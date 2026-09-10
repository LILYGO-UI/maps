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
sequence. If all providers fail at startup, the application retains a world view.
Panning, zooming, and address search remain available.

Location is based on the public IP used to reach the internet, not GPS. A VPN,
proxy, or shared network exit can place the map far from the device. The location
button returns to the most recent successful IP coordinates in the current
session at zoom level 11. If no location has been saved, it starts a new lookup.
A location response received while browsing does not interrupt the user's view.

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

IP lookup exposes the device's public IP to the contacted location providers.
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

Set `LILYGO_MAPS_DISABLE_NETWORK` to exactly `1` to disable location lookup,
address search, and tile downloads:

```sh
LILYGO_MAPS_DISABLE_NETWORK=1 ./build/host-simulator/lilygo-ui-maps
```

Previously cached tiles can still be displayed when you navigate to their area
and zoom level. Missing tiles remain placeholders. This mode does not download
regions in advance, provide offline address search, or persist the previous
session's IP location.
