# QEMU-ABA

LL/SC translation in QEMU causes ABA problem on RISC host. This repo explores various ways to efficiently solve it in software approaches.

## References

1. An artical about LL/SC emulation and ABA problem. [Multi-threaded emulation for QEMU](https://lwn.net/Articles/697265/)
2. [Cross-ISA machine emulation for multicores](http://www.cs.columbia.edu/~cota/pubs/cota_cgo17.pdf)

## Current State

**There are currently 4 solutions implemented in this repo and they are in their branches.**

* HST: Hash table + LL/SC.
* PST: Page fault + LL/SC.
* PST-optimize: actually it's HST-v2, an optimized verison of HST.
* Pico-ST: An solution offered by [PICO](http://www.cs.columbia.edu/~cota/pubs/cota_cgo17.pdf).
* *PMPK: MPK + LL/SC, under development.*


