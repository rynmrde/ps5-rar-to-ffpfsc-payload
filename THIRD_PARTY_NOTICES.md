# Third-party notices

This project is distributed under GPLv3-or-later and retains the notices from the upstream PS5 Web File Manager.

| Component | Upstream | License / obligation |
|---|---|---|
| MkPFS conversion reference | https://github.com/PSBrew/MkPFS | GPL-3.0-or-later; retain copyright and license notices for any ported code. |
| PS5 Web File Manager base | https://github.com/owendswang/ps5-web-file-manager | GPLv3-or-later. |
| libmicrohttpd | https://ftp.gnu.org/gnu/libmicrohttpd/ | LGPL; comply with applicable redistribution and relinking terms. |
| PS5 payload SDK reference | https://github.com/ps5-payload-dev/sdk | GPLv3-or-later, subject to the SDK distribution. |
| websrv and ftpsrv references | https://github.com/ps5-payload-dev/websrv and https://github.com/ps5-payload-dev/ftpsrv | GPLv3-or-later. |
| zftpd reference | https://github.com/seregonwar/zftpd | MIT. |
| ps5-payload-manager reference | https://github.com/itsPLK/ps5-payload-manager | GPLv3. |
| etaHEN reference | https://github.com/LightningMods/etaHEN | GPLv3. |
| ezremote reference | https://github.com/cy33hc/ps5-ezremote-client | GPLv2. |
| Native RAR and 7z extraction | https://github.com/bizkut/unrar-ps5 | Vendored source and upstream notices retained in `third_party/unrar-ps5`; see its `src/license.txt`, `src/acknow.txt`, and `lzma2601` notices. |
| 7-Zip decoder | https://www.7-zip.org/ | Included by the vendored `unrar-ps5` source under its retained `lzma2601` notices. |
| PS5 SceHttp client | https://github.com/ps5-payload-dev/sdk | PS5 system import library used only by the URL downloader target build; the SDK distribution and Sony platform terms apply. |

No Python runtime, shell command execution, or Linux utility is used by the native payload on PS5. The MkPFS Python source remains a reference implementation and is not invoked by the payload.
