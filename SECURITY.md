# Security policy

## Threat model, stated plainly

This is a hobbyist IR blaster for a home network. Being honest about what it
does and does not protect is more useful than a policy that implies more than
the firmware delivers.

**What it protects against:** casual access from other people and devices on
your LAN, when you enable the optional username and password.

**What it does not protect against:** anyone who can reach it on the network
and knows the password, anyone who can read traffic on your LAN, or anyone with
physical access to the board.

Specifically, and by design:

- The web UI, REST API and MQTT all run **unencrypted**. HTTP basic auth sends
  credentials in reachable-if-you-are-listening form. There is no TLS: an ESP32
  can do it, but the certificate management makes a device like this
  substantially less reliable, which is the wrong trade here.
- WiFi and MQTT credentials are stored in **plain NVS**. Anyone who can read
  the flash can read them. Use a dedicated MQTT account with no privileges you
  care about elsewhere.
- Authentication is **off by default**, because most people run this on a
  trusted LAN and a device that locks you out on first boot is worse.
- Firmware OTA is authenticated only by the same basic auth. There is no image
  signing.

### Do not expose this device to the internet

No port forwarding, no DMZ, no public reverse proxy without authentication in
front of it. If you need access from outside your home, use a VPN back to your
LAN. This is the single most important line in this document.

If you must put it behind a reverse proxy, terminate TLS there, require
authentication there, and keep the device itself on an internal network.

---

## Supported versions

The most recent release is the supported one. This is a single-maintainer
hobby project; there are no backported security branches.

| Version | Supported |
|---------|-----------|
| 1.0.x   | ✅ |
| < 1.0   | ❌ |

---

## Reporting a vulnerability

For anything that lets someone bypass authentication, execute code, or read
stored credentials remotely, please report it **privately** first:

- Use GitHub's [private vulnerability reporting][pvr] on this repository
  (Security → Report a vulnerability), or
- email **hammadshamim642@gmail.com** with `SECURITY` in the subject.

[pvr]: https://github.com/hammad42/esp32-ir-blaster/security/advisories/new

Please include what you did, what happened, and the firmware version. A rough
proof of concept helps a lot.

**What to expect:** this is maintained by one person in their spare time, so I
will not promise a response window I cannot keep. I will acknowledge a report
as soon as I see it, and I would rather hear about a problem late than not at
all. Credit in the changelog if you would like it.

Issues that follow directly from the documented design above — "traffic is
unencrypted", "credentials are stored in plaintext", "there is no image
signing" — are known limitations rather than vulnerabilities. Please open a
normal issue if you want to discuss changing one of them; a well-argued case
for TLS or signed images is welcome.
