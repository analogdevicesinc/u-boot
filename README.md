# ADI U-Boot tree

ADI maintains U-Boot support for three categories:

- ADI Digital Signal Processors (ADSP)
    - See [Digital Signal Processors
      (ADSP)](https://analogdevicesinc.github.io/documentation/products/adsp/index.html)
      in the System Level Documentation for more information
    - Branches are prefixed with `adi-u-boot-` and include a mainline U-Boot
      version (e.g. `adi-u-boot-2025.10.y`). Open pull requests against the
      latest branch with a release tag
- AMD/Xilinx
    - ADI devices and peripherals that are integrated with AMD/Xilinx FPGAs
    - Open pull requests against the latest `adi-xlnx-u-boot-` branch
    - Branches are being migrated from
      [u-boot-xlnx](https://github.com/analogdevicesinc/u-boot-xlnx)
      (see [#47](https://github.com/analogdevicesinc/u-boot/issues/47))
- ADI RadioVerse Open Radio Access Network (O-RAN)
    - The ADRV906x 5G base station on a chip
    - Branches are prefixed with `adi-oran-u-boot-`

The Xilinx branches are based on
[u-boot-xlnx](https://github.com/Xilinx/u-boot-xlnx) releases, while all other
categories use [mainline U-Boot releases](https://source.denx.de/u-boot/u-boot).

## CI/CD

This orphan branch is used to maintain Github reusable workflows to be used by
development branches. Since development branches are designed to contain
changes for upstream projects it is better to keep ADI specific changes (like
CI/CD workflows) as minimal as possible on those branches. This also reduces
the need to synchronize CI/CD changes between development branches.
