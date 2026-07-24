# NXP MCU Boot Utility recovery assets

These are the exact Linux/RT1052 recovery assets used on 2026-07-24 to
communicate with the NXP i.MX RT1051 ROM downloader in the NUX NCR-2.

| File | SHA-256 |
|---|---|
| `linux-amd64/sdphost` | `671621fad603cf593e4776d6a1a8a33a2146abbc5c2ae3c1646048b19d6f2263` |
| `linux-amd64/blhost` | `4f3cb30dc6727626c3118e5c378a4ca345185196cf2876ddff2fb740c2d40d6e` |
| `MIMXRT1052/ivt_flashloader.bin` | `c0af776dc30f7312c99eb39c8458c7ba3b28c89f964b9e809d68d232209019a3` |

They were copied without modification from
[JayHeng/NXP-MCUBootUtility](https://github.com/JayHeng/NXP-MCUBootUtility)
commit `84f86164d1d04f7b28b5c25858ce9ba2eae8b216`.

The upstream Apache-2.0 license is preserved as `LICENSE`. The host binaries
target x86-64 Linux. The flashloader loads and executes at `0x20208200`.
