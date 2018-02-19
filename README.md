---
title: oidentd
description: Configurable ident daemon
permalink: /
---

oidentd is an RFC 1413 compliant ident daemon that runs on Linux, FreeBSD,
OpenBSD, NetBSD, DragonFly BSD, and some versions of Darwin and Solaris.

oidentd is used primarily by universities and providers of shell accounts and
public IRC bouncers to fight abuse by allowing servers to identify the owners
of incoming connections.

## Features

- Highly configurable, both globally and on a per-user basis
- Different responses can be sent depending on host and port pairs
- Optional capabilities to allow spoofing of ident responses
- Responses can be hidden or randomized for anonymity
- No configuration necessary
- IP masquerading support
- Full support for IPv6

For a complete list of features, please consult the manual pages
or run `oidentd --help`.

## Download

The most recent version of oidentd will always be available at
[ftp.janikrabe.com](http://ftp.janikrabe.com/pub/oidentd/releases/latest).

All releases are signed with PGP key [0xE3E8FA19DC9A1AA9](key.asc).
It is highly recommended that you verify the signatures of releases
before installing them.

## Contributing

If you're interested in contributing to oidentd, please open an issue or a
pull request on [GitHub](https://github.com/janikrabe/oidentd). You can also
email patches, suggestions, questions, comments, bug reports, et cetera to
[Janik Rabe](https://janikrabe.com) (<info@janikrabe.com>).

## IRC Channel

You can find us on IRC at
irc://chat.freenode.net/[#oidentd](irc://chat.freenode.net/#oidentd).
