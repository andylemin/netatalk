# Introduction to Netatalk

Netatalk is an Open Source software package that can be used to turn a
\*NIX machine into a performant and light-weight file server for
Macintosh computers. It provides a TCP/IP as well as a traditional
AppleTalk transport layer.

Using Netatalk's AFP 3.4 compliant file-server leads to noticeably
higher transmission speeds for macOS clients compared to Samba/NFS,
while providing users with the best possible Mac-like user experience.
It can read and write Mac metadata - macOS extended file attributes as
well as Classic Mac OS resource forks - facilitating mixed environments
of new and old Macs.

Netatalk ships with range of capabilities to accommodate most deployment
environments, including Kerberos, ACLs and LDAP. Modern macOS features
such as Bonjour service discovery, Time Machine backups, and Spotlight
indexed search are provided.

For AppleTalk networks with legacy Macs and Apple IIs, Netatalk provides
a print server, time server, and Apple II NetBoot server. The print
server is fully integrated with CUPS, allowing old Macs to discover and
print to a modern CUPS/AirPrint compatible printer on the network.

Additionally, Netatalk can be configured as an AppleTalk router through
its AppleTalk Network Management daemon, providing both segmentation and
zone names in traditional Macintosh networks.
