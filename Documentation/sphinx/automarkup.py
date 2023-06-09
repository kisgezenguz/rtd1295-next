# SPDX-License-Identifier: GPL-2.0
# Copyright 2019 Jonathan Corbet <corbet@lwn.net>
#
# Apply kernel-specific tweaks after the initial document processing
# has been done.
#
from docutils import nodes
import sphinx
from sphinx import addnodes
if sphinx.version_info[0] < 2 or \
   sphinx.version_info[0] == 2 and sphinx.version_info[1] < 1:
    from sphinx.environment import NoUri
else:
    from sphinx.errors import NoUri
import re
from itertools import chain

#
# Regex nastiness.  Of course.
# Try to identify "function()" that's not already marked up some
# other way.  Sphinx doesn't like a lot of stuff right after a
# :c:func: block (i.e. ":c:func:`mmap()`s" flakes out), so the last
# bit tries to restrict matches to things that won't create trouble.
#
RE_function = re.compile(r'(([\w_][\w\d_]+)\(\))')
RE_type = re.compile(r'(struct|union|enum|typedef)\s+([\w_][\w\d_]+)')

#
# Many places in the docs refer to common system calls.  It is
# pointless to try to cross-reference them and, as has been known
# to happen, somebody defining a function by these names can lead
# to the creation of incorrect and confusing cross references.  So
# just don't even try with these names.
#
Skipfuncs = [ 'open', 'close', 'read', 'write', 'fcntl', 'mmap',
              'select', 'poll', 'fork', 'execve', 'clone', 'ioctl',
              'socket' ]

#
# Find all occurrences of C references (function() and struct/union/enum/typedef
# type_name) and try to replace them with appropriate cross references.
#
def markup_c_refs(docname, app, node):
    class_str = {RE_function: 'c-func', RE_type: 'c-type'}
    reftype_str = {RE_function: 'function', RE_type: 'type'}

    cdom = app.env.domains['c']
    t = node.astext()
    done = 0
    repl = [ ]
    #
    # Sort all C references by the starting position in text
    #
    sorted_matches = sorted(chain(RE_type.finditer(t), RE_function.finditer(t)),
                            key=lambda m: m.start())
    for m in sorted_matches:
        #
        # Include any text prior to match as a normal text node.
        #
        if m.start() > done:
            repl.append(nodes.Text(t[done:m.start()]))
        #
        # Go through the dance of getting an xref out of the C domain
        #
        target = m.group(2)
        target_text = nodes.Text(m.group(0))
        xref = None
        if not (m.re == RE_function and target in Skipfuncs):
            lit_text = nodes.literal(classes=['xref', 'c', class_str[m.re]])
            lit_text += target_text
            pxref = addnodes.pending_xref('', refdomain = 'c',
                                          reftype = reftype_str[m.re],
                                          reftarget = target, modname = None,
                                          classname = None)
            #
            # XXX The Latex builder will throw NoUri exceptions here,
            # work around that by ignoring them.
            #
            try:
                xref = cdom.resolve_xref(app.env, docname, app.builder,
                                         reftype_str[m.re], target, pxref,
                                         lit_text)
            except NoUri:
                xref = None
        #
        # Toss the xref into the list if we got it; otherwise just put
        # the function text.
        #
        if xref:
            repl.append(xref)
        else:
            repl.append(target_text)
        done = m.end()
    if done < len(t):
        repl.append(nodes.Text(t[done:]))
    return repl

def auto_markup(app, doctree, name):
    #
    # This loop could eventually be improved on.  Someday maybe we
    # want a proper tree traversal with a lot of awareness of which
    # kinds of nodes to prune.  But this works well for now.
    #
    # The nodes.literal test catches ``literal text``, its purpose is to
    # avoid adding cross-references to functions that have been explicitly
    # marked with cc:func:.
    #
    for para in doctree.traverse(nodes.paragraph):
        for node in para.traverse(nodes.Text):
            if not isinstance(node.parent, nodes.literal):
                node.parent.replace(node, markup_c_refs(name, app, node))

def setup(app):
    app.connect('doctree-resolved', auto_markup)
    return {
        'parallel_read_safe': True,
        'parallel_write_safe': True,
        }
