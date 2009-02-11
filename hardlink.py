#!/usr/bin/python
#
# Copyright (C) 2008 Julian Andres Klode <jak@jak-linux.org>
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
'''
Hardlink copies of the same file.

This program is partially compatible to http://code.google.com/p/hardlinkpy/,
but some options have changed or are not available anymore.
'''

import filecmp
import os
import re
import sys
import time

from collections import defaultdict
from optparse import OptionParser
from stat import S_ISREG


class File(object):
    '''Represent a file or hardlink

    Attributes:
        path - The path to the file
        hash - The hash of the file (a tuple)
        isreg - Is the file a regular file?
        link_count - The link count of the file
        stat - The result of os.stat(path)
        opts - The commandline options, parsed by optparse

    Please note that the hash attribute is a tuple consisting of multiple
    items, mainly (mode, dev, uid, gid, fname, size, mtime), whereas every
    option except for size and dev may also be None, if this has been set on
    the commandline.
    '''

    __slots__ = 'path', 'hash', 'isreg', 'link_count', 'stat', 'opts', 'linker'

    def __init__(self, fname, opts, linker):
        self.path = fname
        self.opts = opts
        self.stat = os.lstat(fname)
        self.linker = linker
        self.isreg = S_ISREG(self.stat.st_mode)
        self.link_count = self.stat.st_nlink
        self.hash = ((opts.mode and self.stat.st_mode), # Same mode
                     (self.stat.st_dev), # Same device
                     (opts.owner and self.stat.st_uid), # Same user
                     (opts.owner and self.stat.st_gid), # Same group
                     (opts.samename and os.path.basename(fname)), # Same name
                     (self.stat.st_size), # Same size
                     (opts.timestamp and self.stat.st_mtime)) # Same time

    def same_content(self, other):
        '''Return whether to files have identical content'''
        if self.opts.verbose >= 2:
            print 'Comparing', self.path, 'to', other.path
        self.linker.compared += 1
        try:
            return filecmp.cmp(self.path, other.path, 0)
        except EnvironmentError, exc:
            print 'Error: Comparing %s to %s failed' % (self.path, other.path)
            print '      ', exc

    def may_link_to(self, other):
        '''Returns True if the file may be linked to another one.

        If the files have the same hash (ie. at least same size and device),
        and if the path of the files differs and if the size or more than 0,
        and (if content=True) if the content is the same, the file may be
        linked.'''
        return (self.hash == other.hash and self.path != other.path and
                self.stat.st_size and self.same_content(other))

    def is_linked_to(self, other):
        '''Returns True if the files are already hardlinked.'''
        return (self.stat.st_ino == other.stat.st_ino and
                self.stat.st_dev == other.stat.st_dev)

    def link(self, other):
        '''Link the current file to another one'''
        backup = other.path + '.hardlink-%s' % os.getpid()
        if os.path.exists(backup):
            print 'E: Backup file %s already exists, aborting.' % backup
            return
        try:
            if not self.opts.dry_run:
                os.rename(other.path, backup)
        except OSError, err:
            print 'E: Renaming %s to %s failed - ' % (other.path, backup), err
            return
        try:
            if not self.opts.dry_run:
                os.link(self.path, other.path)
        except OSError, err:
            print 'E: Linking %s to %s failed -' % (self.path, other.path), err
            try:
                if not self.opts.dry_run:
                    os.rename(backup, other.path)
            except OSError, err:
                print 'E: Can not restore %s from backup -' % other.path, err
        else:
            if not self.opts.dry_run:
                os.unlink(backup)
            elif self.opts.verbose:
                print '[DryRun]',
            if self.opts.verbose:
                print 'Linking %s to %s (-%s)' % (self.path, other.path,
                                                  format(self.stat.st_size))
            other.link_count = self.link_count = self.link_count + 1
            self.linker.linked += 1
            self.linker.saved += self.stat.st_size
            return True

    def __lt__(self, other):
        '''Less-Than check, respecting options given on command-line.'''
        if ((self.opts.max and other.link_count < self.link_count) or
            (self.opts.max is False and other.link_count > self.link_count) or
            (self.stat.st_mtime > other.stat.st_mtime and not
             self.opts.timestamp)):
            return False
        else:
            return True


class HardLink(object):
    '''The main class for the program.

    Actually, this code uses divide-and-conquer method check for same files.
    For one file, split the list of files into same ones and other ones, and
    recurse into the other ones.'''

    def __init__(self, dirs, opts):
        self.start = time.time()
        self._dirs = dirs
        self.opts = opts
        self.compared = 0
        self.linked = 0
        self.saved = 0
        self.files = self.get_files()
        self.divide_and_conquer()
        self.print_stats()

    def print_stats(self):
        '''Print the statistics at the end of the run'''
        print 'Mode:    ', self.opts.dry_run and 'dry-run' or 'real'
        print 'Files:   ', sum(len(files) for files in self.files.itervalues())
        print 'Linked:  ', self.linked, 'files'
        print 'Compared:', self.compared, 'files'
        print 'Saved:   ', format(self.saved)
        print 'Duration: %.2f seconds' % (time.time() - self.start)

    def divide_and_conquer(self):
        '''Divide and Conquer linking'''
        for files in self.files.itervalues():
            while files:
                if len(files) < 2:
                    break
                remaining = set()
                # Find the master (highest/lowest link/date)
                master = max(files)
                for other in files:
                    # Ignore already linked files
                    if master.is_linked_to(other):
                        continue
                    # If it is allowed to link, do so.
                    elif master.may_link_to(other):
                        master.link(other)
                    # If not, add to remaining
                    else:
                        remaining.add(other)
                files = remaining

    def get_files(self):
        '''Return a dict like {'directory': [File(...)]}'''
        retfiles = defaultdict(list)
        for top in self._dirs:
            for root, _, files in os.walk(top):
                for fname in files:
                    fpath = os.path.join(root, fname)
                    exc = any(pat.search(fpath) for pat in self.opts.exclude)
                    inc = any(pat.search(fpath) for pat in self.opts.include)
                    if ((self.opts.exclude and exc and not inc) or
                        (self.opts.include and not inc)):
                        continue
                    try:
                        mfile = File(fpath, self.opts, self)
                        if mfile.isreg:
                            retfiles[mfile.hash].append(mfile)
                        del mfile
                    except OSError, err:
                        print 'OSError:', err
        return retfiles


def format(bytes):
    '''Format a size, given in bytes'''
    bytes = float(bytes)
    if bytes >= 1024**3:
        return "%.2f GiB" % (bytes/1024**3)
    elif bytes >= 1024**2:
        return "%.2f MiB" % (bytes/1024**2)
    elif bytes >= 1024:
        return "%.2f KiB" % (bytes/1024)
    else:
        return "%d bytes" % bytes


def parse_args():
    '''Parse the command-line options'''
    parser = OptionParser(usage='%prog [options] directory ...',
                          version='hardlink 0.1')
    parser.add_option('-v', '--verbose', action='count', default=0,
                      help='Increase verbosity (repeat for more verbosity)')
    parser.add_option('-n', '--dry-run', action='store_true',
                      help='Modify nothing, just print what would happen')
    parser.add_option('-f', '--respect-name', action='store_true',
                      help='Filenames have to be identical', dest='samename')
    parser.add_option('-p', '--ignore-mode', dest='mode', default=True,
                      action='store_false', help='Ignore changes of file mode')
    parser.add_option('-o', '--ignore-owner', action='store_false',
                      dest='owner', default=True, help='Ignore owner changes')
    parser.add_option('-t', '--ignore-time', action='store_false',
                      dest='timestamp', default=True, help='Ignore timestamps.'
                      ' Will retain the newer timestamp, unless -m or -M is '
                      'given')
    parser.add_option('-m', '--maximize', action='store_true', dest='max',
                      help='Maximize the hardlink count, remove the file with '
                      'lowest hardlink cout')
    parser.add_option('-M', '--minimize', action='store_false', dest='max',
                      help='Reverse the meaning of -m')
    parser.add_option('-x', '--exclude', metavar='REGEXP', action='append',
                      help='Regular expression to exclude files/dirs',
                      default=[])
    parser.add_option('-i', '--include', metavar='REGEXP', action='append',
                      help='Regular expression to include files/dirs',
                      default=[])

    # Parse the arguments
    opts, args = parser.parse_args()
    if not args:
        print >> sys.stderr, 'Error: You must specify at least one directory'
        sys.exit(1)
    # Compile the regular expressions, should speedup matching a bit.
    opts.exclude = tuple(re.compile(pat) for pat in opts.exclude)
    opts.include = tuple(re.compile(pat) for pat in opts.include)
    return opts, args


def main():
    '''The main function. Calls parse_args() and creates HardLink.'''
    opts, dirs = parse_args()
    HardLink(dirs, opts)


if __name__ == '__main__':
    main()
