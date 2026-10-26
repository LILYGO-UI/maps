# Services and Cache

[Back to README](../README.md)

## Online Services

| Purpose | Endpoint |
| --- | --- |
| IP location, first choice | `https://ipapi.co/json/` |
| IP location, first fallback | `https://ipinfo.io/json` |
| IP location, final fallback | `http://ip-api.com/json/` |
| Address search | `https://nominatim.openstreetmap.org/search` |
| Map tiles | `https://tile.openstreetmap.de/{z}/{x}/{y}.png` |

Location providers are tried in order. A failed request, service error, or invalid
coordinates advances to the next provider; a successful response stops the
sequence. If all providers fail at startup and GNSS has no fix, the application retains a world view.
Panning, zooming, and address search remain available.

### GNSS First, IP Fallback

Maps starts a background nRF9151 GNSS reader alongside the IP lookup. A valid
GNSS fix always takes precedence, including when an IP response arrives later.
Fixes expire after 15 seconds without a new valid position. No fix, an invalid
fix, a disconnected modem, or an occupied UART leaves IP as the fallback. The
last successful IP result is kept for the current session. If neither source is
available, Maps retains the current map (or the initial world view).

GNSS positioning does not require internet access; downloading new tiles still
does. A VPN, proxy, or shared network exit can place IP coordinates far from the
device. The status text identifies GNSS and approximate IP positioning.

The location button selects a fresh GNSS fix at zoom 16, otherwise the cached IP
position at zoom 11. If neither exists, it starts a lookup. Subsequent GNSS
updates preserve zoom. Panning, zooming, or searching suspends automatic
recentering until the location button is pressed again; background fixes still
update. Returning to the current location cancels any pending search selection.

The modem must run compatible Nordic Serial LTE Modem (SLM) firmware and have a
GNSS antenna with a clear view of the sky. Maps uses the same 115200 8N1 UART and
lock convention as LILYGO Cellular. The default `/dev/ttyACM0` uses
`/tmp/lilygo_nrf9151_uart.lock`; another application holding that lock causes
Maps to use IP and retry the UART in the background. Cellular currently exposes
no shared location service, so both apps cannot read GNSS from the UART at once.

An existing running GNSS session is reused, but Maps still checks and enables
NMEA output. Firmware that rejects `AT#XNMEA?` is also tried with `AT#XNMEA=1`;
if neither is supported, Maps can receive `#XGNSSPOS` instead. If Maps attaches to an already
sleeping session and receives no fix or wake notification for 15 seconds, it
restarts acquisition in continuous mode. This recovers a completed single-fix
session left by Cellular; it can also replace an older periodic configuration
whose sleep interval exceeds 15 seconds. On exit, Maps stops only acquisition,
NMEA output, and GNSS activation that it started itself. NMEA output is restored
to off only if Maps observed it was off initially.

With NMEA output enabled, a session that produces no NMEA or position data in
its first 15 seconds is restarted. Unfixed GGA/RMC and satellite GSV sentences
count as received data: acquiring a first fix must not cause repeated restarts.
Valid coordinates are required before Maps switches away from IP location;
receiving satellite data alone does not imply a position fix.

| Setting | Meaning |
| --- | --- |
| `LILYGO_MAPS_GNSS_PORT` | Override GNSS serial device |
| `CELLULAR_PORT` | Serial device when the Maps-specific override is unset |
| Neither override | `/dev/ttyACM0` |
| `LILYGO_MAPS_DISABLE_GNSS=1` | Disable the GNSS reader; use IP only |
| `LILYGO_MAPS_GNSS_DEBUG=1` | Log sent AT commands and received UART lines to stderr |

Maps preserves the configured LTE-M/NB-IoT mode and does not deactivate LTE to
change the modem's system mode. If the current active mode cannot enable GNSS,
configure GNSS support in the modem first; Maps continues using IP. GNSS and LTE
share modem time, so heavy cellular traffic can delay satellite acquisition.

UART open/lock failures, rejected or timed-out commands, and unavailable GNSS
system modes are logged with a `[maps-gnss]` prefix. For a detailed device trace,
close Cellular and other serial tools first, then run:

```sh
LILYGO_MAPS_GNSS_DEBUG=1 lilygo-ui-maps 2>/tmp/maps-gnss.log
```

The CM0 default UART is `/dev/ttyACM0`; the K230 reference's `/dev/ttyS3` and GPIO
setup are specific to that board. The [K230 GNSS manager](https://github.com/Xinyuan-LilyGO/T-Display-K230/blob/12206c8043e7995c3d78eae17c729ee42b70b1bd/k230_launcher/k230_phone_ui/src/ui_nrf9151_manager.c#L2728)
also tries `AT#XGNSS=1,0,1` (continuous) before `AT#XGNSS=1,0,0,0` (single fix).
Maps uses that same start sequence, while preserving the configured LTE mode
and using `AT+CFUN=31` when GNSS activation is needed.

Search sends the entered query to Nominatim and centers the map on the first
result. Empty results or request failures leave the current center unchanged.
The service accepts Chinese and English address queries.

### Requests and Privacy

Requests use HTTPS except for the fixed final IP fallback,
`http://ip-api.com/json/`. That free endpoint uses unencrypted HTTP and redirects
are disabled for it. Other requests and their redirects are restricted to HTTPS.

The HTTP client invokes `curl` without shell command concatenation. Requests
include an application User-Agent, an 8-second connection timeout, a 20-second
per-attempt total timeout, one retry, and response size limits. JSON responses
are limited to 256 KiB and tile responses to 1 MiB.

GNSS fixes are processed locally. IP lookup exposes the device's public IP to the contacted location providers.
Address search sends the query to Nominatim, and tile requests identify the map
areas being viewed to the tile provider. Follow each provider's terms and usage
policies, and retain OpenStreetMap attribution.

## Cache Paths

Tiles use Web Mercator / slippy-map coordinates and are stored as
`<cache-root>/<zoom>/<x>/<y>.png`. The cache root is selected in this order:

| Condition | Cache root |
| --- | --- |
| Nonempty `LILYGO_MAPS_CACHE_DIR` | The exact configured directory |
| Running as root | `/root/maps/openstreetmap` |
| Nonempty `XDG_CACHE_HOME` | `$XDG_CACHE_HOME/lilygo-ui-maps/maps/openstreetmap` |
| Nonempty `HOME` | `$HOME/.cache/lilygo-ui-maps/maps/openstreetmap` |
| No home or cache environment | `/tmp/lilygo-ui-maps/maps/openstreetmap` |

The root-user path matches the T-Display-K230 Launcher's offline tile directory
convention. This is a cache layout compatibility detail, not a device build target.

Set a custom cache root before launching the simulator:

```sh
LILYGO_MAPS_CACHE_DIR="$HOME/maps/openstreetmap" \
  ./build/host-simulator/lilygo-ui-maps
```

The directory must be writable. Use a dedicated directory whose final component
is `openstreetmap`: clearing the cache recursively removes its contents, and the
application refuses to clear a cache root with any other final directory name.

### Validation and Clearing

Accepted tiles must be `256 x 256` PNGs no larger than 1 MiB, with the expected
PNG signature, IHDR dimensions, and IEND marker. Downloads are written to a
temporary `.part` file and renamed when complete. Existing valid files are reused
when their tiles enter the viewport.

The interface shows cache usage and asks for confirmation before clearing tiles.
Clearing advances a cache epoch so earlier in-flight downloads cannot repopulate
the cleared cache. Cache errors appear in the status overlay. See the
[architecture overview](00-overview.md) for threading and consistency details.

## Offline Operation

Set `LILYGO_MAPS_DISABLE_NETWORK` to exactly `1` to disable IP lookup, address
search, and tile downloads. GNSS positioning remains available:

```sh
LILYGO_MAPS_DISABLE_NETWORK=1 ./build/host-simulator/lilygo-ui-maps
```

Previously cached tiles can still be displayed when you navigate to their area
and zoom level. Missing tiles remain placeholders. This mode does not download
regions in advance, provide offline address search, or persist the previous
session's IP location.
