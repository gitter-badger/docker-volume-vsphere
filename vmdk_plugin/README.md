This directory contains code for client (docker) side of docker-volume-vsphere.

* ***.go** - GO code implementing Docker Plugin API (used docker/go-pliugins-helper)
* **vmdkops** - Client Module (in GO) with C interface for communcating over VMCI
* **utils** - misc. GO helper modules
* **package** - info needed for building RPM or DEB and then starting service 