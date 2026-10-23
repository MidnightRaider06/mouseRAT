# mouseRAT

A proof of concept of a simple application-layer protocol over TCP for shell commands and screenshots between user-controlled VMs.

Features:

- Binary message framing and multiple client connections
- Nonblocking network I/O and bounded buffering
- Background screenshot file writes

The Linux server is tested on an Ubuntu VM. The Windows client is tested on Windows 10.

This is a lab demonstration, not malware. It provides no stealth, persistence, or access-control bypass and is not designed to function as malware.
