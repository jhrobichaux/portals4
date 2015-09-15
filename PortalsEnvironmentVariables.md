# Introduction #

This page lists all of the available environment variables for the Portals 4 Reference Implementation.


# Details #

**PTL\_IFACE\_NAME**: The name of the network interface that you wish to use. Default names starting with eth, ib or en are automatically found. Any others need to be explicitly set with this variable, e.g. ens1, qib0.

**PTL\_DEBUG**: Set to 0/1 to disable/enable Portals4 warning messages

**PTL\_LOG\_LEVEL**: Set to 0 to disable trace messages, set up to 3 for more detailed messages.

**PTL\_ENABLE\_MEM**: Set to 0/1 to disable/enable the local memory transport (if you compiled in support for it).

**PTL\_DISABLE\_MEM\_REG\_CACHE**: Set to 0/1 to activate/deactivate the IB memory registration cache. Disabling it no longer requires ummunotify, and the implementation does not keep a registered memory cache.