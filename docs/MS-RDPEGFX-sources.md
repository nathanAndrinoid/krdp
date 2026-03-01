# MS-RDPEGFX – Source Code and Packages

References for implementing or extending the RDP Graphics Pipeline (MS-RDPEGFX) in KRDP.

## Specification

- **[MS-RDPEGFX] Remote Desktop Protocol: Graphics Pipeline Extension**  
  https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/da5c75f9-cd99-450c-98c4-014a496942b0  
  - Authoritative protocol spec (PDUs, codecs, capability negotiation).
  - PDF/DOCX: linked from the same page under “Published Version”.
  - ClearCodec RLE: [3.3.8.1.1 ClearCodec Run-Length Encoding](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/9f49c46a-9ad9-4139-902f-22edce0042db).

## FreeRDP (reference implementation)

- **Repository:** https://github.com/FreeRDP/FreeRDP  
- **RDPGFX channel (client + server):** `channels/rdpgfx/`
  - `rdpgfx_common.c` / `rdpgfx_common.h` – shared header/PDU helpers.
  - `server/` – server-side RDPGFX (e.g. `rdpgfx_main.c`: WireToSurface, AVC420, CapsConfirm, etc.).
  - `client/` – client-side decode and caps handling.
- **Codecs (in libfreerdp):** `libfreerdp/codec/`
  - **ClearCodec:** `clear.c`, `include/freerdp/codec/clear.h`
    - `clear_compress()` – **not implemented** (returns error, “TODO: not implemented!”).
    - `clear_decompress()` – implemented; useful as reference for ClearCodec wire format.
  - **RemoteFX (RFX):** `rfx_encode.c`, `rfx_decode.c`, `rfx.c`, etc.
    - `rfx_encode_message()` / `rfx_encode_messages()` – encoder exists; RDPGFX uses CAVIDEO (codec ID 0x0003); wire format may differ from raw RFX.
  - **NSC:** `nsc.c`, `nsc_encode.c` – NSC encode/decode (used inside ClearCodec in some paths).
  - **Planar:** `planar.c` – planar bitmap codec.
  - **Progressive:** `progressive.c`, `progressive.h` – progressive codec.
  - **H.264:** `h264.c`, `h264_ffmpeg.c`, etc. – AVC420/AVC444.

**Packages (e.g. Arch):** `freerdp` – provides `libfreerdp`, `libfreerdp-server`, and headers under `/usr/include/freerdp3/`. KRDP already links these; no extra package needed for server + codec APIs.

## GNOME Remote Desktop (GRD)

- **Repository:** https://github.com/GNOME/gnome-remote-desktop (mirror; upstream: https://gitlab.gnome.org/GNOME/gnome-remote-desktop)  
- Uses FreeRDP for RDP; session and graphics pipeline logic in `src/` (e.g. `grd-session-rdp.c`, `grd-rdp-*.c`).  
- Useful for how a real RDP server wires capture, frame rate, and RDPGFX together; not a separate codec library.

## Summary for KRDP

| Goal                         | Where to look |
|-----------------------------|----------------|
| RDPGFX server behaviour     | FreeRDP `channels/rdpgfx/server/` + MS-RDPEGFX spec |
| ClearCodec **decode** (wire format) | FreeRDP `libfreerdp/codec/clear.c` (`clear_decompress`) |
| ClearCodec **encode**       | **Missing** in FreeRDP; would need implementation (e.g. from MS-RDPEGFX 3.3.8.1.1) or a contribution to FreeRDP |
| RemoteFX encode             | FreeRDP `libfreerdp/codec/rfx_encode.c` (RFX); RDPGFX CAVIDEO mapping may need extra glue |
| AVC420 / H.264              | FreeRDP server already uses it; KRDP uses same path for non-fallback clients |

No extra system package is required beyond `freerdp` (and your existing libav/FFmpeg for the current H.264→uncompressed fallback). Implementing a ClearCodec encoder would mean either contributing to FreeRDP’s `clear_compress()` or adding a small encoder in KRDP that emits the wire format expected by `clear_decompress()`.

---

## Best alternative for compression (fallback path)

For clients that do **not** support H.264 (e.g. iPad), the server currently sends **uncompressed** RDPGFX. To reduce bandwidth with compression, the practical options are:

| Option | Pros | Cons |
|--------|------|------|
| **1. ClearCodec** | Required by MS-RDPEGFX; all clients support it. Good compression for simple/static content (RLE, glyph cache, residual). | FreeRDP’s `clear_compress()` is **not implemented** (stub). Encoder would need to be implemented from the spec and FreeRDP’s `clear_decompress()` (wire format). ClearCodec is more than “simple RLE”: it has bands, glyph cache, residual, subcodecs (NSC, RLEX). |
| **2. RemoteFX (CAVIDEO)** | Encoder exists in FreeRDP (`rfx_encode_message()` in `libfreerdp/codec/rfx_encode.c`). | RDPGFX server path in FreeRDP does not currently send CAVIDEO; you’d need to wire RFX output into RDPGFX surface commands (codec ID `RDPGFX_CODECID_CAVIDEO`). Some clients (e.g. iPad) may not advertise or support CAVIDEO. |
| **3. Planar** | Codec exists in FreeRDP. | Same as RFX: server RDPGFX integration and client support are uncertain. |
| **4. Keep uncompressed + tune** | No new codec work. | Use the existing **Fallback scale** (1/2/4) and **Fallback max FPS** in the KCM to reduce bandwidth (current approach). |

**Recommendation:** The **best alternative that adds real compression** is **implementing a ClearCodec encoder**, because (1) every RDPGFX client must support it, and (2) FreeRDP already has the decoder, so the wire format is defined. The work is implementing the encoder (e.g. in FreeRDP’s `clear_compress()` or a minimal encoder in KRDP that produces the same bitstream). Short term, **configurable fallback (scale + FPS)** remains the most practical way to improve behaviour without implementing a new codec.
