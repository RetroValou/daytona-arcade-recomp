# Run on PS Vita

## Utilities (once)

| What | Where | Needed |
| --- | --- | --- |
| [VitaShell](https://github.com/TheOfficialFloW/VitaShell) | installs the `.vpk` files, copies the ROMs over USB/FTP | yes |
| `libshacccg.suprx` (the shader compiler, e.g. via [ShaRKBR33D](https://github.com/Rinnegatamante/ShaRKBR33D)) | `ur0:data/libshacccg.suprx` | yes |
| An overclock plugin | for CPU CLOCK 500 MHz (otherwise 444) | no |

## Games

Install the game's `.vpk` with VitaShell, then copy your own MAME ROM set, unchanged
(ZIP, full merged), into its folder:

| Game | File on the Vita |
| --- | --- |
| Daytona USA | `ux0:data/daytona93/daytona93.zip` |
| Daytona USA (Revision A) | `ux0:data/daytona/daytona.zip` |
| Sega Rally Championship | `ux0:data/srallyc/srallyc.zip` (MAME's `srallycb.zip`, renamed) |
| Virtua Fighter 2 | `ux0:data/vf2/vf2.zip` |
| Virtua Cop | `ux0:data/vcop/vcop.zip` |
| Virtua Cop 2 | `ux0:data/vcop2/vcop2.zip` |
| Manx TT Superbike | `ux0:data/manxttc/manxtt.zip` (or `manxttc.zip`) |
| Dead or Alive | `ux0:data/doaa/doa.zip` (or `doaa.zip`) |

- ZIP only: repack a `.7z` as `.zip` with the same file names.
- Every file's CRC is checked: a set that is not the right one is refused.
- The settings (`vita.cfg`) and saves are written to the same folder.
