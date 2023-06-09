// SPDX-License-Identifier: GPL-2.0
#include <linux/err.h>
#include <linux/igmp.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/rculist.h>
#include <linux/skbuff.h>
#include <linux/if_ether.h>
#include <net/ip.h>
#include <net/netlink.h>
#include <net/switchdev.h>
#if IS_ENABLED(CONFIG_IPV6)
#include <net/ipv6.h>
#include <net/addrconf.h>
#endif

#include "br_private.h"

static int br_rports_fill_info(struct sk_buff *skb, struct netlink_callback *cb,
			       struct net_device *dev)
{
	struct net_bridge *br = netdev_priv(dev);
	struct net_bridge_port *p;
	struct nlattr *nest, *port_nest;

	if (!br->multicast_router || hlist_empty(&br->router_list))
		return 0;

	nest = nla_nest_start_noflag(skb, MDBA_ROUTER);
	if (nest == NULL)
		return -EMSGSIZE;

	hlist_for_each_entry_rcu(p, &br->router_list, rlist) {
		if (!p)
			continue;
		port_nest = nla_nest_start_noflag(skb, MDBA_ROUTER_PORT);
		if (!port_nest)
			goto fail;
		if (nla_put_nohdr(skb, sizeof(u32), &p->dev->ifindex) ||
		    nla_put_u32(skb, MDBA_ROUTER_PATTR_TIMER,
				br_timer_value(&p->multicast_router_timer)) ||
		    nla_put_u8(skb, MDBA_ROUTER_PATTR_TYPE,
			       p->multicast_router)) {
			nla_nest_cancel(skb, port_nest);
			goto fail;
		}
		nla_nest_end(skb, port_nest);
	}

	nla_nest_end(skb, nest);
	return 0;
fail:
	nla_nest_cancel(skb, nest);
	return -EMSGSIZE;
}

static void __mdb_entry_fill_flags(struct br_mdb_entry *e, unsigned char flags)
{
	e->state = flags & MDB_PG_FLAGS_PERMANENT;
	e->flags = 0;
	if (flags & MDB_PG_FLAGS_OFFLOAD)
		e->flags |= MDB_FLAGS_OFFLOAD;
	if (flags & MDB_PG_FLAGS_FAST_LEAVE)
		e->flags |= MDB_FLAGS_FAST_LEAVE;
}

static void __mdb_entry_to_br_ip(struct br_mdb_entry *entry, struct br_ip *ip)
{
	memset(ip, 0, sizeof(struct br_ip));
	ip->vid = entry->vid;
	ip->proto = entry->addr.proto;
	if (ip->proto == htons(ETH_P_IP))
		ip->u.ip4 = entry->addr.u.ip4;
#if IS_ENABLED(CONFIG_IPV6)
	else
		ip->u.ip6 = entry->addr.u.ip6;
#endif
}

static int __mdb_fill_srcs(struct sk_buff *skb,
			   struct net_bridge_port_group *p)
{
	struct net_bridge_group_src *ent;
	struct nlattr *nest, *nest_ent;

	if (hlist_empty(&p->src_list))
		return 0;

	nest = nla_nest_start(skb, MDBA_MDB_EATTR_SRC_LIST);
	if (!nest)
		return -EMSGSIZE;

	hlist_for_each_entry_rcu(ent, &p->src_list, node,
				 lockdep_is_held(&p->port->br->multicast_lock)) {
		nest_ent = nla_nest_start(skb, MDBA_MDB_SRCLIST_ENTRY);
		if (!nest_ent)
			goto out_cancel_err;
		switch (ent->addr.proto) {
		case htons(ETH_P_IP):
			if (nla_put_in_addr(skb, MDBA_MDB_SRCATTR_ADDRESS,
					    ent->addr.u.ip4)) {
				nla_nest_cancel(skb, nest_ent);
				goto out_cancel_err;
			}
			break;
#if IS_ENABLED(CONFIG_IPV6)
		case htons(ETH_P_IPV6):
			if (nla_put_in6_addr(skb, MDBA_MDB_SRCATTR_ADDRESS,
					     &ent->addr.u.ip6)) {
				nla_nest_cancel(skb, nest_ent);
				goto out_cancel_err;
			}
			break;
#endif
		default:
			nla_nest_cancel(skb, nest_ent);
			continue;
		}
		if (nla_put_u32(skb, MDBA_MDB_SRCATTR_TIMER,
				br_timer_value(&ent->timer))) {
			nla_nest_cancel(skb, nest_ent);
			goto out_cancel_err;
		}
		nla_nest_end(skb, nest_ent);
	}

	nla_nest_end(skb, nest);

	return 0;

out_cancel_err:
	nla_nest_cancel(skb, nest);
	return -EMSGSIZE;
}

static int __mdb_fill_info(struct sk_buff *skb,
			   struct net_bridge_mdb_entry *mp,
			   struct net_bridge_port_group *p)
{
	bool dump_srcs_mode = false;
	struct timer_list *mtimer;
	struct nlattr *nest_ent;
	struct br_mdb_entry e;
	u8 flags = 0;
	int ifindex;

	memset(&e, 0, sizeof(e));
	if (p) {
		ifindex = p->port->dev->ifindex;
		mtimer = &p->timer;
		flags = p->flags;
	} else {
		ifindex = mp->br->dev->ifindex;
		mtimer = &mp->timer;
	}

	__mdb_entry_fill_flags(&e, flags);
	e.ifindex = ifindex;
	e.vid = mp->addr.vid;
	if (mp->addr.proto == htons(ETH_P_IP))
		e.addr.u.ip4 = mp->addr.u.ip4;
#if IS_ENABLED(CONFIG_IPV6)
	if (mp->addr.proto == htons(ETH_P_IPV6))
		e.addr.u.ip6 = mp->addr.u.ip6;
#endif
	e.addr.proto = mp->addr.proto;
	nest_ent = nla_nest_start_noflag(skb,
					 MDBA_MDB_ENTRY_INFO);
	if (!nest_ent)
		return -EMSGSIZE;

	if (nla_put_nohdr(skb, sizeof(e), &e) ||
	    nla_put_u32(skb,
			MDBA_MDB_EATTR_TIMER,
			br_timer_value(mtimer))) {
		nla_nest_cancel(skb, nest_ent);
		return -EMSGSIZE;
	}
	switch (mp->addr.proto) {
	case htons(ETH_P_IP):
		dump_srcs_mode = !!(p && mp->br->multicast_igmp_version == 3);
		break;
#if IS_ENABLED(CONFIG_IPV6)
	case htons(ETH_P_IPV6):
		dump_srcs_mode = !!(p && mp->br->multicast_mld_version == 2);
		break;
#endif
	}
	if (dump_srcs_mode &&
	    (__mdb_fill_srcs(skb, p) ||
	     nla_put_u8(skb, MDBA_MDB_EATTR_GROUP_MODE, p->filter_mode))) {
		nla_nest_cancel(skb, nest_ent);
		return -EMSGSIZE;
	}

	nla_nest_end(skb, nest_ent);

	return 0;
}

static int br_mdb_fill_info(struct sk_buff *skb, struct netlink_callback *cb,
			    struct net_device *dev)
{
	int idx = 0, s_idx = cb->args[1], err = 0, pidx = 0, s_pidx = cb->args[2];
	struct net_bridge *br = netdev_priv(dev);
	struct net_bridge_mdb_entry *mp;
	struct nlattr *nest, *nest2;

	if (!br_opt_get(br, BROPT_MULTICAST_ENABLED))
		return 0;

	nest = nla_nest_start_noflag(skb, MDBA_MDB);
	if (nest == NULL)
		return -EMSGSIZE;

	hlist_for_each_entry_rcu(mp, &br->mdb_list, mdb_node) {
		struct net_bridge_port_group *p;
		struct net_bridge_port_group __rcu **pp;

		if (idx < s_idx)
			goto skip;

		nest2 = nla_nest_start_noflag(skb, MDBA_MDB_ENTRY);
		if (!nest2) {
			err = -EMSGSIZE;
			break;
		}

		if (!s_pidx && mp->host_joined) {
			err = __mdb_fill_info(skb, mp, NULL);
			if (err) {
				nla_nest_cancel(skb, nest2);
				break;
			}
		}

		for (pp = &mp->ports; (p = rcu_dereference(*pp)) != NULL;
		      pp = &p->next) {
			if (!p->port)
				continue;
			if (pidx < s_pidx)
				goto skip_pg;

			err = __mdb_fill_info(skb, mp, p);
			if (err) {
				nla_nest_end(skb, nest2);
				goto out;
			}
skip_pg:
			pidx++;
		}
		pidx = 0;
		s_pidx = 0;
		nla_nest_end(skb, nest2);
skip:
		idx++;
	}

out:
	cb->args[1] = idx;
	cb->args[2] = pidx;
	nla_nest_end(skb, nest);
	return err;
}

static int br_mdb_valid_dump_req(const struct nlmsghdr *nlh,
				 struct netlink_ext_ack *extack)
{
	struct br_port_msg *bpm;

	if (nlh->nlmsg_len < nlmsg_msg_size(sizeof(*bpm))) {
		NL_SET_ERR_MSG_MOD(extack, "Invalid header for mdb dump request");
		return -EINVAL;
	}

	bpm = nlmsg_data(nlh);
	if (bpm->ifindex) {
		NL_SET_ERR_MSG_MOD(extack, "Filtering by device index is not supported for mdb dump request");
		return -EINVAL;
	}
	if (nlmsg_attrlen(nlh, sizeof(*bpm))) {
		NL_SET_ERR_MSG(extack, "Invalid data after header in mdb dump request");
		return -EINVAL;
	}

	return 0;
}

static int br_mdb_dump(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct net_device *dev;
	struct net *net = sock_net(skb->sk);
	struct nlmsghdr *nlh = NULL;
	int idx = 0, s_idx;

	if (cb->strict_check) {
		int err = br_mdb_valid_dump_req(cb->nlh, cb->extack);

		if (err < 0)
			return err;
	}

	s_idx = cb->args[0];

	rcu_read_lock();

	cb->seq = net->dev_base_seq;

	for_each_netdev_rcu(net, dev) {
		if (dev->priv_flags & IFF_EBRIDGE) {
			struct br_port_msg *bpm;

			if (idx < s_idx)
				goto skip;

			nlh = nlmsg_put(skb, NETLINK_CB(cb->skb).portid,
					cb->nlh->nlmsg_seq, RTM_GETMDB,
					sizeof(*bpm), NLM_F_MULTI);
			if (nlh == NULL)
				break;

			bpm = nlmsg_data(nlh);
			memset(bpm, 0, sizeof(*bpm));
			bpm->ifindex = dev->ifindex;
			if (br_mdb_fill_info(skb, cb, dev) < 0)
				goto out;
			if (br_rports_fill_info(skb, cb, dev) < 0)
				goto out;

			cb->args[1] = 0;
			nlmsg_end(skb, nlh);
		skip:
			idx++;
		}
	}

out:
	if (nlh)
		nlmsg_end(skb, nlh);
	rcu_read_unlock();
	cb->args[0] = idx;
	return skb->len;
}

static int nlmsg_populate_mdb_fill(struct sk_buff *skb,
				   struct net_device *dev,
				   struct net_bridge_mdb_entry *mp,
				   struct net_bridge_port_group *pg,
				   int type)
{
	struct nlmsghdr *nlh;
	struct br_port_msg *bpm;
	struct nlattr *nest, *nest2;

	nlh = nlmsg_put(skb, 0, 0, type, sizeof(*bpm), 0);
	if (!nlh)
		return -EMSGSIZE;

	bpm = nlmsg_data(nlh);
	memset(bpm, 0, sizeof(*bpm));
	bpm->family  = AF_BRIDGE;
	bpm->ifindex = dev->ifindex;
	nest = nla_nest_start_noflag(skb, MDBA_MDB);
	if (nest == NULL)
		goto cancel;
	nest2 = nla_nest_start_noflag(skb, MDBA_MDB_ENTRY);
	if (nest2 == NULL)
		goto end;

	if (__mdb_fill_info(skb, mp, pg))
		goto end;

	nla_nest_end(skb, nest2);
	nla_nest_end(skb, nest);
	nlmsg_end(skb, nlh);
	return 0;

end:
	nla_nest_end(skb, nest);
cancel:
	nlmsg_cancel(skb, nlh);
	return -EMSGSIZE;
}

static size_t rtnl_mdb_nlmsg_size(struct net_bridge_port_group *pg)
{
	size_t nlmsg_size = NLMSG_ALIGN(sizeof(struct br_port_msg)) +
			    nla_total_size(sizeof(struct br_mdb_entry)) +
			    nla_total_size(sizeof(u32));
	struct net_bridge_group_src *ent;
	size_t addr_size = 0;

	if (!pg)
		goto out;

	switch (pg->addr.proto) {
	case htons(ETH_P_IP):
		if (pg->port->br->multicast_igmp_version == 2)
			goto out;
		addr_size = sizeof(__be32);
		break;
#if IS_ENABLED(CONFIG_IPV6)
	case htons(ETH_P_IPV6):
		if (pg->port->br->multicast_mld_version == 1)
			goto out;
		addr_size = sizeof(struct in6_addr);
		break;
#endif
	}

	/* MDBA_MDB_EATTR_GROUP_MODE */
	nlmsg_size += nla_total_size(sizeof(u8));

	/* MDBA_MDB_EATTR_SRC_LIST nested attr */
	if (!hlist_empty(&pg->src_list))
		nlmsg_size += nla_total_size(0);

	hlist_for_each_entry(ent, &pg->src_list, node) {
		/* MDBA_MDB_SRCLIST_ENTRY nested attr +
		 * MDBA_MDB_SRCATTR_ADDRESS + MDBA_MDB_SRCATTR_TIMER
		 */
		nlmsg_size += nla_total_size(0) +
			      nla_total_size(addr_size) +
			      nla_total_size(sizeof(u32));
	}
out:
	return nlmsg_size;
}

struct br_mdb_complete_info {
	struct net_bridge_port *port;
	struct br_ip ip;
};

static void br_mdb_complete(struct net_device *dev, int err, void *priv)
{
	struct br_mdb_complete_info *data = priv;
	struct net_bridge_port_group __rcu **pp;
	struct net_bridge_port_group *p;
	struct net_bridge_mdb_entry *mp;
	struct net_bridge_port *port = data->port;
	struct net_bridge *br = port->br;

	if (err)
		goto err;

	spin_lock_bh(&br->multicast_lock);
	mp = br_mdb_ip_get(br, &data->ip);
	if (!mp)
		goto out;
	for (pp = &mp->ports; (p = mlock_dereference(*pp, br)) != NULL;
	     pp = &p->next) {
		if (p->port != port)
			continue;
		p->flags |= MDB_PG_FLAGS_OFFLOAD;
	}
out:
	spin_unlock_bh(&br->multicast_lock);
err:
	kfree(priv);
}

static void br_mdb_switchdev_host_port(struct net_device *dev,
				       struct net_device *lower_dev,
				       struct net_bridge_mdb_entry *mp,
				       int type)
{
	struct switchdev_obj_port_mdb mdb = {
		.obj = {
			.id = SWITCHDEV_OBJ_ID_HOST_MDB,
			.flags = SWITCHDEV_F_DEFER,
		},
		.vid = mp->addr.vid,
	};

	if (mp->addr.proto == htons(ETH_P_IP))
		ip_eth_mc_map(mp->addr.u.ip4, mdb.addr);
#if IS_ENABLED(CONFIG_IPV6)
	else
		ipv6_eth_mc_map(&mp->addr.u.ip6, mdb.addr);
#endif

	mdb.obj.orig_dev = dev;
	switch (type) {
	case RTM_NEWMDB:
		switchdev_port_obj_add(lower_dev, &mdb.obj, NULL);
		break;
	case RTM_DELMDB:
		switchdev_port_obj_del(lower_dev, &mdb.obj);
		break;
	}
}

static void br_mdb_switchdev_host(struct net_device *dev,
				  struct net_bridge_mdb_entry *mp, int type)
{
	struct net_device *lower_dev;
	struct list_head *iter;

	netdev_for_each_lower_dev(dev, lower_dev, iter)
		br_mdb_switchdev_host_port(dev, lower_dev, mp, type);
}

void br_mdb_notify(struct net_device *dev,
		   struct net_bridge_mdb_entry *mp,
		   struct net_bridge_port_group *pg,
		   int type)
{
	struct br_mdb_complete_info *complete_info;
	struct switchdev_obj_port_mdb mdb = {
		.obj = {
			.id = SWITCHDEV_OBJ_ID_PORT_MDB,
			.flags = SWITCHDEV_F_DEFER,
		},
		.vid = mp->addr.vid,
	};
	struct net *net = dev_net(dev);
	struct sk_buff *skb;
	int err = -ENOBUFS;

	if (pg) {
		if (mp->addr.proto == htons(ETH_P_IP))
			ip_eth_mc_map(mp->addr.u.ip4, mdb.addr);
#if IS_ENABLED(CONFIG_IPV6)
		else
			ipv6_eth_mc_map(&mp->addr.u.ip6, mdb.addr);
#endif
		mdb.obj.orig_dev = pg->port->dev;
		switch (type) {
		case RTM_NEWMDB:
			complete_info = kmalloc(sizeof(*complete_info), GFP_ATOMIC);
			if (!complete_info)
				break;
			complete_info->port = pg->port;
			complete_info->ip = mp->addr;
			mdb.obj.complete_priv = complete_info;
			mdb.obj.complete = br_mdb_complete;
			if (switchdev_port_obj_add(pg->port->dev, &mdb.obj, NULL))
				kfree(complete_info);
			break;
		case RTM_DELMDB:
			switchdev_port_obj_del(pg->port->dev, &mdb.obj);
			break;
		}
	} else {
		br_mdb_switchdev_host(dev, mp, type);
	}

	skb = nlmsg_new(rtnl_mdb_nlmsg_size(pg), GFP_ATOMIC);
	if (!skb)
		goto errout;

	err = nlmsg_populate_mdb_fill(skb, dev, mp, pg, type);
	if (err < 0) {
		kfree_skb(skb);
		goto errout;
	}

	rtnl_notify(skb, net, 0, RTNLGRP_MDB, NULL, GFP_ATOMIC);
	return;
errout:
	rtnl_set_sk_err(net, RTNLGRP_MDB, err);
}

static int nlmsg_populate_rtr_fill(struct sk_buff *skb,
				   struct net_device *dev,
				   int ifindex, u32 pid,
				   u32 seq, int type, unsigned int flags)
{
	struct br_port_msg *bpm;
	struct nlmsghdr *nlh;
	struct nlattr *nest;

	nlh = nlmsg_put(skb, pid, seq, type, sizeof(*bpm), 0);
	if (!nlh)
		return -EMSGSIZE;

	bpm = nlmsg_data(nlh);
	memset(bpm, 0, sizeof(*bpm));
	bpm->family = AF_BRIDGE;
	bpm->ifindex = dev->ifindex;
	nest = nla_nest_start_noflag(skb, MDBA_ROUTER);
	if (!nest)
		goto cancel;

	if (nla_put_u32(skb, MDBA_ROUTER_PORT, ifindex))
		goto end;

	nla_nest_end(skb, nest);
	nlmsg_end(skb, nlh);
	return 0;

end:
	nla_nest_end(skb, nest);
cancel:
	nlmsg_cancel(skb, nlh);
	return -EMSGSIZE;
}

static inline size_t rtnl_rtr_nlmsg_size(void)
{
	return NLMSG_ALIGN(sizeof(struct br_port_msg))
		+ nla_total_size(sizeof(__u32));
}

void br_rtr_notify(struct net_device *dev, struct net_bridge_port *port,
		   int type)
{
	struct net *net = dev_net(dev);
	struct sk_buff *skb;
	int err = -ENOBUFS;
	int ifindex;

	ifindex = port ? port->dev->ifindex : 0;
	skb = nlmsg_new(rtnl_rtr_nlmsg_size(), GFP_ATOMIC);
	if (!skb)
		goto errout;

	err = nlmsg_populate_rtr_fill(skb, dev, ifindex, 0, 0, type, NTF_SELF);
	if (err < 0) {
		kfree_skb(skb);
		goto errout;
	}

	rtnl_notify(skb, net, 0, RTNLGRP_MDB, NULL, GFP_ATOMIC);
	return;

errout:
	rtnl_set_sk_err(net, RTNLGRP_MDB, err);
}

static bool is_valid_mdb_entry(struct br_mdb_entry *entry)
{
	if (entry->ifindex == 0)
		return false;

	if (entry->addr.proto == htons(ETH_P_IP)) {
		if (!ipv4_is_multicast(entry->addr.u.ip4))
			return false;
		if (ipv4_is_local_multicast(entry->addr.u.ip4))
			return false;
#if IS_ENABLED(CONFIG_IPV6)
	} else if (entry->addr.proto == htons(ETH_P_IPV6)) {
		if (ipv6_addr_is_ll_all_nodes(&entry->addr.u.ip6))
			return false;
#endif
	} else
		return false;
	if (entry->state != MDB_PERMANENT && entry->state != MDB_TEMPORARY)
		return false;
	if (entry->vid >= VLAN_VID_MASK)
		return false;

	return true;
}

static int br_mdb_parse(struct sk_buff *skb, struct nlmsghdr *nlh,
			struct net_device **pdev, struct br_mdb_entry **pentry)
{
	struct net *net = sock_net(skb->sk);
	struct br_mdb_entry *entry;
	struct br_port_msg *bpm;
	struct nlattr *tb[MDBA_SET_ENTRY_MAX+1];
	struct net_device *dev;
	int err;

	err = nlmsg_parse_deprecated(nlh, sizeof(*bpm), tb,
				     MDBA_SET_ENTRY_MAX, NULL, NULL);
	if (err < 0)
		return err;

	bpm = nlmsg_data(nlh);
	if (bpm->ifindex == 0) {
		pr_info("PF_BRIDGE: br_mdb_parse() with invalid ifindex\n");
		return -EINVAL;
	}

	dev = __dev_get_by_index(net, bpm->ifindex);
	if (dev == NULL) {
		pr_info("PF_BRIDGE: br_mdb_parse() with unknown ifindex\n");
		return -ENODEV;
	}

	if (!(dev->priv_flags & IFF_EBRIDGE)) {
		pr_info("PF_BRIDGE: br_mdb_parse() with non-bridge\n");
		return -EOPNOTSUPP;
	}

	*pdev = dev;

	if (!tb[MDBA_SET_ENTRY] ||
	    nla_len(tb[MDBA_SET_ENTRY]) != sizeof(struct br_mdb_entry)) {
		pr_info("PF_BRIDGE: br_mdb_parse() with invalid attr\n");
		return -EINVAL;
	}

	entry = nla_data(tb[MDBA_SET_ENTRY]);
	if (!is_valid_mdb_entry(entry)) {
		pr_info("PF_BRIDGE: br_mdb_parse() with invalid entry\n");
		return -EINVAL;
	}

	*pentry = entry;
	return 0;
}

static int br_mdb_add_group(struct net_bridge *br, struct net_bridge_port *port,
			    struct br_ip *group, struct br_mdb_entry *entry)
{
	struct net_bridge_mdb_entry *mp;
	struct net_bridge_port_group *p;
	struct net_bridge_port_group __rcu **pp;
	unsigned long now = jiffies;
	int err;

	mp = br_mdb_ip_get(br, group);
	if (!mp) {
		mp = br_multicast_new_group(br, group);
		err = PTR_ERR_OR_ZERO(mp);
		if (err)
			return err;
	}

	/* host join */
	if (!port) {
		/* don't allow any flags for host-joined groups */
		if (entry->state)
			return -EINVAL;
		if (mp->host_joined)
			return -EEXIST;

		br_multicast_host_join(mp, false);
		br_mdb_notify(br->dev, mp, NULL, RTM_NEWMDB);

		return 0;
	}

	for (pp = &mp->ports;
	     (p = mlock_dereference(*pp, br)) != NULL;
	     pp = &p->next) {
		if (p->port == port)
			return -EEXIST;
		if ((unsigned long)p->port < (unsigned long)port)
			break;
	}

	p = br_multicast_new_port_group(port, group, *pp, entry->state, NULL,
					MCAST_EXCLUDE);
	if (unlikely(!p))
		return -ENOMEM;
	rcu_assign_pointer(*pp, p);
	if (entry->state == MDB_TEMPORARY)
		mod_timer(&p->timer, now + br->multicast_membership_interval);
	br_mdb_notify(br->dev, mp, p, RTM_NEWMDB);

	return 0;
}

static int __br_mdb_add(struct net *net, struct net_bridge *br,
			struct br_mdb_entry *entry)
{
	struct br_ip ip;
	struct net_device *dev;
	struct net_bridge_port *p = NULL;
	int ret;

	if (!netif_running(br->dev) || !br_opt_get(br, BROPT_MULTICAST_ENABLED))
		return -EINVAL;

	if (entry->ifindex != br->dev->ifindex) {
		dev = __dev_get_by_index(net, entry->ifindex);
		if (!dev)
			return -ENODEV;

		p = br_port_get_rtnl(dev);
		if (!p || p->br != br || p->state == BR_STATE_DISABLED)
			return -EINVAL;
	}

	__mdb_entry_to_br_ip(entry, &ip);

	spin_lock_bh(&br->multicast_lock);
	ret = br_mdb_add_group(br, p, &ip, entry);
	spin_unlock_bh(&br->multicast_lock);
	return ret;
}

static int br_mdb_add(struct sk_buff *skb, struct nlmsghdr *nlh,
		      struct netlink_ext_ack *extack)
{
	struct net *net = sock_net(skb->sk);
	struct net_bridge_vlan_group *vg;
	struct net_bridge_port *p = NULL;
	struct net_device *dev, *pdev;
	struct br_mdb_entry *entry;
	struct net_bridge_vlan *v;
	struct net_bridge *br;
	int err;

	err = br_mdb_parse(skb, nlh, &dev, &entry);
	if (err < 0)
		return err;

	br = netdev_priv(dev);

	if (entry->ifindex != br->dev->ifindex) {
		pdev = __dev_get_by_index(net, entry->ifindex);
		if (!pdev)
			return -ENODEV;

		p = br_port_get_rtnl(pdev);
		if (!p || p->br != br || p->state == BR_STATE_DISABLED)
			return -EINVAL;
		vg = nbp_vlan_group(p);
	} else {
		vg = br_vlan_group(br);
	}

	/* If vlan filtering is enabled and VLAN is not specified
	 * install mdb entry on all vlans configured on the port.
	 */
	if (br_vlan_enabled(br->dev) && vg && entry->vid == 0) {
		list_for_each_entry(v, &vg->vlan_list, vlist) {
			entry->vid = v->vid;
			err = __br_mdb_add(net, br, entry);
			if (err)
				break;
		}
	} else {
		err = __br_mdb_add(net, br, entry);
	}

	return err;
}

static int __br_mdb_del(struct net_bridge *br, struct br_mdb_entry *entry)
{
	struct net_bridge_mdb_entry *mp;
	struct net_bridge_port_group *p;
	struct net_bridge_port_group __rcu **pp;
	struct br_ip ip;
	int err = -EINVAL;

	if (!netif_running(br->dev) || !br_opt_get(br, BROPT_MULTICAST_ENABLED))
		return -EINVAL;

	__mdb_entry_to_br_ip(entry, &ip);

	spin_lock_bh(&br->multicast_lock);
	mp = br_mdb_ip_get(br, &ip);
	if (!mp)
		goto unlock;

	/* host leave */
	if (entry->ifindex == mp->br->dev->ifindex && mp->host_joined) {
		br_multicast_host_leave(mp, false);
		err = 0;
		br_mdb_notify(br->dev, mp, NULL, RTM_DELMDB);
		if (!mp->ports && netif_running(br->dev))
			mod_timer(&mp->timer, jiffies);
		goto unlock;
	}

	for (pp = &mp->ports;
	     (p = mlock_dereference(*pp, br)) != NULL;
	     pp = &p->next) {
		if (!p->port || p->port->dev->ifindex != entry->ifindex)
			continue;

		if (p->port->state == BR_STATE_DISABLED)
			goto unlock;

		br_multicast_del_pg(mp, p, pp);
		err = 0;
		break;
	}

unlock:
	spin_unlock_bh(&br->multicast_lock);
	return err;
}

static int br_mdb_del(struct sk_buff *skb, struct nlmsghdr *nlh,
		      struct netlink_ext_ack *extack)
{
	struct net *net = sock_net(skb->sk);
	struct net_bridge_vlan_group *vg;
	struct net_bridge_port *p = NULL;
	struct net_device *dev, *pdev;
	struct br_mdb_entry *entry;
	struct net_bridge_vlan *v;
	struct net_bridge *br;
	int err;

	err = br_mdb_parse(skb, nlh, &dev, &entry);
	if (err < 0)
		return err;

	br = netdev_priv(dev);

	if (entry->ifindex != br->dev->ifindex) {
		pdev = __dev_get_by_index(net, entry->ifindex);
		if (!pdev)
			return -ENODEV;

		p = br_port_get_rtnl(pdev);
		if (!p || p->br != br || p->state == BR_STATE_DISABLED)
			return -EINVAL;
		vg = nbp_vlan_group(p);
	} else {
		vg = br_vlan_group(br);
	}

	/* If vlan filtering is enabled and VLAN is not specified
	 * delete mdb entry on all vlans configured on the port.
	 */
	if (br_vlan_enabled(br->dev) && vg && entry->vid == 0) {
		list_for_each_entry(v, &vg->vlan_list, vlist) {
			entry->vid = v->vid;
			err = __br_mdb_del(br, entry);
		}
	} else {
		err = __br_mdb_del(br, entry);
	}

	return err;
}

void br_mdb_init(void)
{
	rtnl_register_module(THIS_MODULE, PF_BRIDGE, RTM_GETMDB, NULL, br_mdb_dump, 0);
	rtnl_register_module(THIS_MODULE, PF_BRIDGE, RTM_NEWMDB, br_mdb_add, NULL, 0);
	rtnl_register_module(THIS_MODULE, PF_BRIDGE, RTM_DELMDB, br_mdb_del, NULL, 0);
}

void br_mdb_uninit(void)
{
	rtnl_unregister(PF_BRIDGE, RTM_GETMDB);
	rtnl_unregister(PF_BRIDGE, RTM_NEWMDB);
	rtnl_unregister(PF_BRIDGE, RTM_DELMDB);
}
