.. SPDX-License-Identifier: GPL-2.0-or-later

ADSP OTP memory
===============

The ADI ADSP OTP driver (``drivers/misc/adi-otp*``, ``CONFIG_ADI_OTP``) reads,
programs and locks the one-time programmable (OTP) memory of Analog Devices
ADSP processors, currently the ADSP-SC58x. OTP bits can be changed from 0 to 1
once and never back; programming and locking are irreversible.

It provides the ``fuse`` commands (``CONFIG_CMD_FUSE``):

============================================  =======================================
``fuse read <bank> <word> [<cnt>]``           read OTP words
``fuse sense <bank> <word> [<cnt>]``          the same (there are no shadow registers)
``fuse cmp <bank> <word> <hex>``              compare an OTP word with a value
``fuse readm <bank> <word> <addr> [<cnt>]``   read OTP words to memory
``fuse prog [-y] <bank> <word> <hex>...``     program words, under the rules below
``fuse writebuff [-y] <addr>``                program a buffer (``CONFIG_CMD_FUSE_WRITEBUFF``)
``fuse override``                             not supported
============================================  =======================================

Bank *n* is the *n*-th ADI OTP device, bank 0 on the ADSP-SC58x. The
``fuse`` command addresses 32-bit words (word = OTP byte offset / 4, with the
offsets of the SoC's OTP layout, see `ADSP-SC58x`_); a word's value is its 4
bytes taken as a little-endian number. Reserved words read as 0. This is only
how the generic command addresses the OTP: programming works in the SoC's
write units (16-bit half-words on the ADSP-SC58x), and ``fuse prog`` only
programs the non-zero units of the word it is given.

``fuse writebuff`` is the way to provision a part: one buffer describes
everything to program, and, optionally, locking the part for secure boot.
Nothing is programmed unless the whole buffer is valid and the result is
safe (see `Rules`_).

Typical use, with a buffer built by ``tools/adi_otp_buf.py`` (see `Tool`_)::

    $ tools/adi_otp_buf.py build provision.json -o check.bin --dry-run
    $ tools/adi_otp_buf.py build provision.json -o provision.bin

    => fuse read 0 0 0xe4                  # current content
    => tftp ${loadaddr} check.bin
    => fuse writebuff -y ${loadaddr}       # checks and reports, programs nothing
    => tftp ${loadaddr} provision.bin
    => fuse writebuff ${loadaddr}          # asks for confirmation, then programs
    => reset

``fuse writebuff`` asks for confirmation before it looks at the buffer, so it
asks for a dry run too; ``-y`` skips the question, which is only safe for a
buffer known to be a dry run.

Buffer format
-------------

All values are little-endian. A buffer is a header followed by entries
(``include/adi_otp.h``).

Header (32 bytes):

======  =====  ===============  =========================================
Offset  Size   Name             Value
======  =====  ===============  =========================================
0x00    4      ``magic``        0x50544f41 ("AOTP")
0x04    2      ``version``      1
0x06    2      ``hdr_size``     32
0x08    4      ``soc_id``       the SoC family, e.g. 0x38354353 ("SC58")
0x0c    4      ``flags``        see below
0x10    4      ``total_size``   whole buffer in bytes, header included,
                                a multiple of 4, at most 64 KiB
0x14    4      ``num_entries``  number of entries
0x18    4      ``crc32``        CRC-32 (as zlib's ``crc32()``) of
                                ``total_size`` bytes with this field 0
0x1c    4      ``reserved``     0
======  =====  ===============  =========================================

Each entry (12 bytes, followed by ``length`` bytes of data, padded with zeros
to a multiple of 4 bytes):

======  =====  ============  =================================================
Offset  Size   Name          Value
======  =====  ============  =================================================
0x00    2      ``type``      1: DATA, 2: COUNTER
0x02    2      ``reserved``  0
0x04    4      ``offset``    DATA: OTP byte offset, a multiple of the
                             write size. COUNTER: offset of the counter
                             field
0x08    4      ``length``    DATA: number of data bytes, a non-zero
                             multiple of the write size. COUNTER: 4
======  =====  ============  =================================================

* **DATA** requests bytes at an OTP offset, in OTP address order: the
  first data byte goes to ``offset``, the next to ``offset + 1``, and so on.
  ``offset`` and ``length`` must be multiples of the SoC's write size, the
  unit the OTP is programmed in (2 bytes, a 16-bit half-word, on the
  ADSP-SC58x). An entry may cover part of a field, a whole field or several
  fields. Write units that are zero request nothing, so fields can be left
  out or filled in part.
* **COUNTER** requests a counter value (a ``__le32``), e.g. the
  anti-rollback counter, which the OTP stores in an encoding of its own.

The entries must end exactly at ``total_size``. Unknown versions, flags,
entry types and non-zero reserved fields are rejected, so that a buffer
meant for a newer format is never half understood.

Flags:

=========  ====================  ==============================================
Bit        Name                  Meaning
=========  ====================  ==============================================
0          LOCK                  lock the part, last, after everything else has
                                 been programmed and verified
1          DRY_RUN               check and report what would be done, program
                                 nothing
2-15                             reserved, 0
16-31                            SoC specific (ADSP-SC58x: `Locking`_)
=========  ====================  ==============================================

Rules
-----

Every request (buffer or ``fuse prog``) is checked as a whole, against the
part's current OTP content, before anything is programmed:

* The part must be open: not locked, not "unlocked" (failure analysis), and
  its lock not programmed (it takes effect at the next reset).
* The OTP is programmed in write units (on the ADSP-SC58x 16-bit
  half-words, each with its own ECC), and each unit can be programmed once.
  For each requested unit: 0 or equal to the current value is skipped;
  otherwise the current value must be blank (0), or the request fails
  (``-EEXIST``). A request repeated after succeeding changes nothing.
* Requested units must lie in writable fields, outside the SoC's guarded
  bytes; counters only through COUNTER entries, which only count up. Two
  entries may not request different values for a unit.
* Value fields (e.g. ``bootModeDisable``) must stay in range.
* The SoC's rules for the content after the request must hold, e.g. that the
  part still boots, and, with LOCK, that secure boot has what it needs.

Then the SoC programs the request in the order it needs (on the ADSP-SC58x
field by field, each read back before the next, then the counter), the
whole OTP area is read back and compared with the expected content, and
only then, with LOCK, is the part locked. An error while programming stops
the request: fields programmed before stay programmed and the failing one
may be partly programmed; read the OTP before deciding what to do next.
Diagnostics name fields and byte offsets, never the content of data fields,
since they may hold keys (counts and clock register values are shown).

ADSP-SC58x
----------

The write size is 2 bytes: each 16-bit half-word has its own ECC. All OTP
access goes through the boot ROM API (``otp_get()``,
``otp_get_data()``, ``otp_pgm()``, ``otp_lock()``, ``otp_cfg()``), which
implements the OTP's redundancy and ECC scheme. U-Boot calls it through
typed function pointers to the ROM's jump table (``adi_sc58x_rom_otp_api``,
a ``struct adi_rom_otp_api`` in ``include/adi_rom.h``, defined in
``arch/arm/mach-sc5xx/sc58x.c``) with its 1:1
translation table; only the ROM's section is made executable, for each call
(``adi_rom_enter()``, ``adi_rom_exit()``, ``arch/arm/mach-sc5xx/rom.c``). The
device tree node follows the Linux
binding (``doc/device-tree-bindings/nvmem/adi,adsp-otp.yaml``); its
``adi,rcu`` phandle gives the Reset Control Unit, and the CGU registers come
from the ``adi,sc58x-clocks`` node.

OTP area (offsets as ``ROM_OTP_<name>`` in CCES ``cdefSC58x_rom_otpLayout.h``):

=====  ======================  =====  =============================================
Off.   Field                   Bytes  Notes
=====  ======================  =====  =============================================
0x000  ``huk``                 32
0x020  ``DTCP_key_ecc``        160
0x0c0  ``DTCP_key_cont``       40
0x0e8  ``DTCP_key_dev``        128
0x168  ``pvt_128key0``-``3``   4x16   0x168, 0x178, 0x188, 0x198
0x1a8  ``ek``                  32
0x1c8  ``secure_emu_key``      16     debug key of a locked part
0x1d8                                 reserved
0x200  ``public_key0``         64
0x240  ``public_key1``         64
0x280  ``boot_info``           64     ``ADI_ROM_OTP_BOOT_INFO``; word 15 guarded
0x2c0  ``antiroll_nv_cntr``    64     counter 0-32, COUNTER entries only
0x300  ``gp1``                 64
0x340  ``bootModeDisable``     4      8-bit value in the low byte
0x344  ``preboot_ddr_cfg``     48
0x374  ``stageID``             8      48 bits; the half-word at 0x37a is guarded
0x37c                                 reserved
0x380  ``fsn``                 16     factory serial number, read-only
=====  ======================  =====  =============================================

``bootModeDisable`` is the low byte of the word at 0x340: CCES gives it as
that word (``ROM_OTP_bootModeDisable``) and as an 8-bit value in
``otp_data``; the HRM's layout table shows byte 0x343. ``antiroll_nv_cntr``
reads raw (``fuse read``); a COUNTER entry sets its value.

Boot safety
~~~~~~~~~~~

Requests are refused if they could leave the part unable to boot. The
boot mode, clock and DMC rules can each be turned off with a SoC flag,
``BOOT_MODE_UNCHECKED``, ``CGU_UNCHECKED`` and ``DMC_UNCHECKED``, for
settings verified some other way; a setting that does not work can then
keep the part from ever booting again.

* ``boot_info`` word 15 (0x2bc-0x2bf) is never written: it holds
  ``otpTiming``, the OTP read timing override (``ROM_OTP_otpTiming`` in
  CCES); a wrong value makes the OTP, and so the boot, fail.
* ``bootModeDisable`` may not disable the boot mode the part boots in
  (``RCU0_STAT.BMODE``; its bit disables that boot mode), and the boot
  command of that mode (its word among ``boot_info`` words 4-7, the boot
  commands) may not be changed, since it cannot be tested from U-Boot. With ``SYS_BMODE`` = 0 (no boot, e.g. JTAG)
  neither may be written.
* Preboot clock settings (``boot_info`` words 0-3) must equal the registers
  the running system uses (``CGU0_CTL``, ``CGU0_DIV``, ``CGU0_CLKOUTSEL``,
  ``CGU0/1_OSCWDCTL``, compared on the bits the preboot sets): only settings
  known to work are written. To program other clocks, boot U-Boot with them
  first (an SPL whose ``CONFIG_CGU0_*`` settings are the ones wanted), or
  set ``CGU_UNCHECKED``.
* The preboot DMC configuration may not be enabled (``dmcEn``), nor changed
  while enabled: it cannot be validated from U-Boot, and the ROM is limited to
  a 300 MHz DCLK with it (anomaly 20000073). ``preboot_ddr_cfg`` can be
  written while it is not enabled; ``dmcInv`` can always be set.

Locking
~~~~~~~

A locked part only boots secure boot streams, authenticated with one key,
chosen as EE-366 and EE-384 describe: the first public key not
invalidated in ``boot_info`` (``pubkey0Inv``, ``pubkey1Inv``) or, with
``decryptOnlyEn`` set and not invalidated, the first private key not
invalidated (``privkey0Inv``-``privkey3Inv``). LOCK is refused unless that key
is programmed, in the OTP or in the same buffer.

A locked part cannot read its factory serial number (anomaly 20000108); the
workaround is to store it in OTP that stays readable. LOCK is refused unless
``gp1`` holds the serial number (at any word offset), with two mutually
exclusive SoC flags, ``FSN_COPY`` and ``FSN_IGNORE``.

A locked part only grants secure debug access to a debugger that presents
the debug key (HRM, boot ROM chapter, "Secure Debug Access"). On every boot
of a locked part the ROM loads ``secure_emu_key`` into
``TAPC_SDBGKEY0``-``3``, OTP word *n* (0x1c8 + 4 *n*, little-endian) into
``TAPC_SDBGKEYn``, and sets the key valid; it loads nothing if all of the key
is ones. A debugger presents its key as four 32-bit values: in OpenOCD
``USERKEY0``-``3`` (set as ``USERKEYn_Proc<n>``; 0 by default in
``target/adspsc58x.cfg``; shifted in by ``target/adspsc5xx.tcl``), with
``USERKEYn`` matching ``TAPC_SDBGKEYn``. So:

* a blank ``secure_emu_key`` is a debug key of all zeros, the debuggers'
  default (OpenOCD's ``USERKEY0``-``3`` default to 0): anyone could debug the
  locked part. LOCK is refused unless the ``EMU_KEY_BLANK`` flag is set;
* all ones loads no key: the locked part has no debug access unless the
  application loads a key (the HRM's CAUTION in "Secure Debug Access"). U-Boot
  warns.

SoC flags:

======  =======================  ===========================================
Bit     Name                     Meaning
======  =======================  ===========================================
16      ``FSN_COPY``             copy the serial number to the first blank
                                 16-byte slot of ``gp1`` (0x300, 0x310, ...)
                                 unless it is there already
17      ``FSN_IGNORE``           lock without the serial number in ``gp1``
18      ``EMU_KEY_BLANK``        lock with a blank ``secure_emu_key``, i.e.
                                 the all-zero debug key
19      ``BOOT_MODE_UNCHECKED``  no boot mode rule: ``bootModeDisable`` and
                                 the boot commands are written as requested
20      ``CGU_UNCHECKED``        no clock rule: ``boot_info`` clock settings
                                 may differ from the running system's
21      ``DMC_UNCHECKED``        no DMC rule: ``dmcEn`` and
                                 ``preboot_ddr_cfg`` are written as requested
======  =======================  ===========================================

U-Boot cannot check that the boot streams are signed for that key, nor that
they do without init blocks (not supported on a locked part), nor their
rollback ID against ``antiroll_nv_cntr``: verify secure boot before locking.

Other cores
~~~~~~~~~~~

Simultaneous OTP accesses by several cores can hang a core (anomaly
20000084). The SHARC+ cores run code of their own once the ARM boot ROM has
released them (the ROM's idle loop, a boot stream's application or one started
by U-Boot), so before programming they are held in reset, with the HRM's core
reset sequence; the next reset releases them.

ROM behaviour and anomalies
~~~~~~~~~~~~~~~~~~~~~~~~~~~

* 20000039, the OTP API does not report OTP errors: ``OTPC_STAT.ADDRERR`` and
  ``OTPC_PMC_STAT[5:4]`` are checked after every ROM call.
* The ROM waits for the programming controller (PMC) without a timeout: before
  every programming call ``otp_cfg()`` reconfigures the OTPC and the PMC must
  be idle.
* ``otp_pgm()`` always also handles the anti-rollback counter, and fails if
  the count it is given is below the current one: the current count is
  passed unless the counter is being advanced.
* 20000129, a leaky bit makes a programmed half-word read back as 0 (not in
  the anomaly list; CCES ``adi/sys/SC5xx_anomaly_macros_rtl.h``): every field
  is read back after programming.
* 20000082, unaligned half-word reads of non-cacheable memory: the driver only
  makes 32-bit accesses to the OTPC and the ROM reads the OTP itself.
* 20000003, SPU MMR access faults: the driver does not use ``SPU_SECURECHK``;
  U-Boot runs as a secure master.

Tool
----

``tools/adi_otp_buf.py`` (Python 3, no dependencies) builds buffers from a
JSON description and decodes them::

    tools/adi_otp_buf.py build SPEC.json -o BUF.bin [--dry-run]
    tools/adi_otp_buf.py show BUF.bin
    tools/adi_otp_buf.py fields [--soc sc58x]

Example description::

    {
      "soc": "sc58x",
      "flags": ["lock", "fsn-copy"],
      "fields": {
        "public_key0": { "file": "pubkey.bin" },
        "pvt_128key0": { "file": "enckey.bin" },
        "secure_emu_key": [0, 17, 34, 51, 68, 85, 102, 119,
                           136, 153, 170, 187, 204, 221, 238, 255],
        "gp1": { "offset": 14, "data": ["0xbe", "0xef"] },
        "boot_info": { "bcfg": { "pubkey1Inv": 1 } },
        "bootModeDisable": ["uart"],
        "antiroll_nv_cntr": 1
      }
    }

Data is always an array of bytes in OTP address order, lowest address
first; each byte is an integer (0-255) or a ``"0x.."`` string. There is no
other form, so there is no byte order to get wrong. The example programs
``gp1`` bytes 14 and 15 (OTP 0x30e, 0x30f) with 0xbe and 0xef, so a byte
dump such as Linux's ``hexdump -C`` of the NVMEM device shows ``be ef`` at
0x30e. ``fuse read 0 0xc3`` shows the 32-bit word holding them, which reads
little-endian: ``efbe0000``. Data must start at, and
be a whole number of, the SoC's write units (2 bytes on the ADSP-SC58x).

* ``soc``: ``sc58x`` (or ``sc582``-``sc589``).
* ``flags``: ``lock``, ``dry-run``, ``fsn-copy``, ``fsn-ignore``,
  ``emu-key-blank``, ``boot-mode-unchecked``, ``cgu-unchecked``,
  ``dmc-unchecked``.
* Field values: an array of bytes, written from the start of the field
  (possibly shorter than it); ``{"offset": N, "data": [bytes]}`` for bytes
  at byte offset N within the field; or ``{"file": "path"}`` (optionally
  with ``"offset"``) for the bytes of a binary file, in file order, the path
  being relative to the description. Key fields take adi_signtool's files
  directly, see `Keys from adi_signtool`_.
* Values rather than data, given as integers, which the tool turns into
  bytes:

  * ``boot_info``: also an object of named sub-fields in the groups
    ``cgu``, ``bcmd``, ``bcfg`` and ``val14`` (``ADI_ROM_OTP_BOOT_INFO``);
    see ``fields`` for the names;
  * ``bootModeDisable``: an integer or a list of ``spi-master``,
    ``spi-slave``, ``linkport``, ``uart``;
  * ``antiroll_nv_cntr``: the target count.

* ``raw``: an array of ``{"offset": N, "data": [bytes]}`` or
  ``{"offset": N, "file": "path"}`` at absolute OTP offsets.

The tool checks the layout, the write units and the guarded bytes; U-Boot
checks everything against the part, so run the ``--dry-run`` buffer first.

Keys from adi_signtool
~~~~~~~~~~~~~~~~~~~~~~

The key fields take the files that adi_signtool (CCES) produces or uses, as
they are; their bytes go into the OTP unchanged, because the ADSP-SC58x boot
ROM reads both kinds of key in the order adi_signtool writes them:

=========================  ==================================================
Field                      File
=========================  ==================================================
``public_key0``,           the public key for authentication (BLp, BLx, BLw):
``public_key1``            ``adi_signtool getkey -type BLKey -key
                           keychain.der -outfile pubkey.bin`` (56 bytes), or
                           the C header of ``-type BLKeyC``
``pvt_128key0``-``3``      an AES-128 key file (16 bytes): the ``-enckey``
                           file for BLx, the ``-wrapkey`` file for BLw
=========================  ==================================================

* The public key is the ECDSA-224 point Qx then Qy, 28 bytes each, most
  significant byte first. The ROM copies the 56 bytes it reads from the
  field into its key structure and loads each coordinate most significant
  byte first, so the field holds the file's bytes from its start, the last 8
  bytes blank. The same bytes result from programming the ``BLKeyC`` array
  with ``adi_rom_otp_pgm()``, which programs memory as it is. The tool checks
  that the key is a point on the secp224r1 curve, which a reordered,
  truncated or wrong file is not, and refuses DER keypairs (they hold the
  private key) and ECDSA-256 keys.
* The ROM copies an AES key's 16 bytes from the field into the AES engine's
  key record as they are, as it does with the stream's IV, so the field holds
  the key file's bytes.
* Secure boot uses the first key not invalidated in ``boot_info`` (see
  `Locking`_): program the key into that field, and verify secure boot with a
  stream signed (and encrypted) with it before locking.

``secure_emu_key`` also takes the debug key as a debugger presents it::

    "secure_emu_key": { "userkey": ["0x12345678", "0x9abcdef0",
                                    "0x00000000", "0x5f5f5f5f"] }

``USERKEY0`` first; each value goes to 0x1c8 + 4 *n* little-endian (here
``78 56 34 12 f0 de bc 9a 00 00 00 00 5f 5f 5f 5f``), which the ROM loads into
``TAPC_SDBGKEYn``. The tool refuses an all-zero key and prints the matching
OpenOCD arguments (``-c "set USERKEY0_Proc0 0x12345678; ..."``); keep them
secret.

Driver structure and adding a SoC family
----------------------------------------

``drivers/misc/adi-otp.h`` is the interface between the two parts of the
driver:

* ``adi-otp.c`` is the U-Boot driver and knows no SoC. Its compatible table
  maps each compatible to a SoC family's ``struct adi_otp_soc``. It parses
  requests, applies the rules above that every OTP shares from the SoC's
  data (fields, write size, guarded bytes), and owns the sequence: check,
  report, apply, verify the whole OTP area, lock.
* ``adi-otp-<soc>.c`` is everything about one SoC family: its data (buffer
  SoC id and flags, write size, field table, guarded bytes) and its
  operations (``struct adi_otp_soc_ops``): ``read()``, ``read_counter()``
  and ``lock_state()``; ``prepare()``, which may add data to a request
  before it is checked (e.g. the FSN copy); ``check()``, its own rules,
  which can only refuse more than the generic ones; ``apply()``, which
  programs a checked request in whatever order and with whatever
  precautions the SoC needs; and ``lock()``, which ``adi-otp.c`` only calls
  once everything has been verified.
* ``adi-otp-fuse.c`` maps the ``fuse`` commands to the driver.

The ADSP-SC58x code calls its boot ROM through ``include/adi_rom.h``. On
sandbox, ``adi-otp-sc58x-sandbox.c`` emulates that ROM, so the unit tests
(``ut dm adi_otp*``, ``test/dm/adi_otp.c``) run the real SC58x rules,
including ROM failures and the anomaly workarounds.

Adding a SoC family takes an ``adi-otp-<soc>.c`` with its data, its
compatibles in ``adi-otp.c``, and a ``CONFIG_ADI_OTP_<SOC>`` option. The
data starts with the family's ``struct adi_otp_soc``, which is all that
``adi-otp.c`` sees, and goes on with whatever the SoC code needs: the
ADSP-SC58x's (``struct adi_sc58x_soc``) adds its boot ROM API.
