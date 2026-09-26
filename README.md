Aspia Sync
==========

A fork of [Aspia](https://github.com/dchapyshev/aspia) **2.7.0** that lets a team share one address book through the Aspia Router.

Aspia is a remote desktop, file transfer and system information tool by **Dmitry Chapyshev**. With it you can run your own NAT traversal infrastructure (Router and Relay) and connect by ID, or connect directly. All of that is his work; this fork adds to it.

About this fork
---------------

This fork starts from the 2.7.0 release (commit `488a77e13`, May 2024) and **does not follow upstream**. The original project has moved on to 3.x; nothing from there is merged here, and nothing from here is meant to go back. Fixes made upstream after 2.7.0, security fixes included, do not reach this fork on their own.

It exists for one reason: a team that runs Aspia 2.7 needed its address books to stay in step without moving everyone to a new major version.

If you do not need shared address books, use the [original project](https://github.com/dchapyshev/aspia).

What is added
-------------

### Shared address books

Several consoles keep one address book in step through the Router. An edit made in one console reaches the others; an edit made offline goes out when the connection comes back.

- **End-to-end encrypted.** Records are sealed with ChaCha20-Poly1305 under a key derived with scrypt from a team password. The Router stores them but cannot read them: it sees the shape of the tree, record IDs and timestamps, not names, addresses, comments, logins or passwords. A known value sealed with the key lets a console catch a wrong password before it writes anything.
- **Opt-in per book.** A book stays an ordinary local file until synchronization is turned on for it, and can be turned back into one.
- **Edits go out as they are made**, imports included, and colleagues' edits arrive while the console is open.
- **Records have identity.** Every computer and group gets a GUID when it is created, so renaming or moving it keeps it the same record. Older books get GUIDs on first open.
- **Three-way merge.** When two people change the same record, fields changed on only one side are taken silently. A person is asked only when both sides changed the same field to different values, and a conflict holds back that one record, not the rest.
- **Local changes are found by comparison**, not by tracking every place the book is edited, so no edit can be forgotten.
- **Deletions are tombstones**, one per record of a deleted subtree, so a console that was offline for a while does not bring deleted computers back. The Router keeps them for 60 days and sweeps them daily.
- **Built to fail safely.** Repeated batches are applied once; the book file is written atomically; a Router database restored from a backup is detected and stops the exchange instead of letting books drift apart quietly.
- **Only consoles can get a book.** The address book channel refuses and closes for every other session type, so a compromised host that authenticates to the Router still cannot fetch it.
- **Personal things stay personal**: the Router credentials in the book and which folders are expanded are not shared.

In the console: **Synchronization → Synchronization Settings...** opens a wizard that creates a shared book or joins one. The first console to join an empty shared book fills it with its own records; every console after that takes the shared book as it is. Before anything is written, the wizard shows what the book will look like and keeps a copy of the file next to it (`<name>.before-sync.aab`, never overwritten), so whatever only one person had can be imported back. A joined book reconnects by itself when it is opened. The synchronization window shows the state, what is waiting to be sent, and lets a person settle a conflict by keeping their version or taking the colleagues' one.

### Console

- Search across the whole book by words, best match first: by name, folder or ID.
- A dark theme (**View → Dark Theme**).
- The address book file is written through a temporary file, so a crash or a power loss leaves either the old book or the new one, never a truncated file.

### Router

- Stores shared address books next to its existing database.
- `--console` runs it in the foreground and stops it cleanly on Ctrl+C.
- Builds for Linux as a `.deb` package with a systemd service. The private key and the database are readable by root only.

### Build and tests

- Build scripts for Windows (`tools/build`) that locate Visual Studio themselves and check for missing components up front.
- The console and the router, which had no tests before, now have unit tests, including an end-to-end test that drives the real router store and service through the console's sync engine.
- vcpkg is pinned to a known commit.

Platforms
---------

| Component | Platforms |
|---|---|
| Console, Client, Host | Windows x64 |
| Router | Windows x64, Linux x86_64 (Ubuntu 24.04 or newer) |
| Relay | Windows x64 |

macOS and the non-router Linux builds of the original were removed.

Compatibility
-------------

- Existing `.aab` files open as before. The GUIDs added to them are ignored by a stock 2.7 console, but a stock console that **saves** the file drops them, so do not edit one book with both.
- Hosts, clients and relays of Aspia 2.7 keep working with this fork. Their code and the connection protocol are those of 2.7.0; the Router only gained new messages and a channel of its own for address books.
- A Console or Client refuses a Host, and the Router Manager refuses a Router, whose version is newer than its own. A stock 2.7.0 Console compares the build number too, so it refuses every build of this fork. Update the Consoles and Clients first, then the Router and the Hosts. This fork compares the release alone (2.8.0, not 2.8.0.4926), so builds of one release from different commits accept each other.

Releases
--------

Pushing a tag `vX.Y.Z` that matches the version in `CMakeLists.txt` builds the Windows installers and the Linux router package on GitHub Actions and attaches them to a draft release. The workflow can also be run by hand from the Actions tab to build without releasing. Nothing runs on an ordinary push.

The first build compiles Qt5 and the rest of the dependencies and takes hours; later ones reuse the cache.

Building
--------

See [tools/build/README.md](tools/build/README.md) for Windows and for the Linux router. In short:

```
git submodule update --init
tools\build\verify.cmd
```

`verify.cmd` configures, builds and runs the tests.

Licensing
---------

GNU General Public License 3, as the original. See [LICENSE.md](LICENSE.md).

Aspia is copyright © 2016-2024 Dmitry Chapyshev. Questions about this fork go to this repository's [issues](https://github.com/teoritty/aspia-sync/issues), not to the original author.
