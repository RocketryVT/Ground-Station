# tracking.py  --  Geodetic helpers for antenna-tracker gimbal control
# All angles in degrees unless noted.  Pure math, no hardware deps.

from math import radians, degrees, sin, cos, atan2, sqrt, asin

# Earth radius in metres (WGS-84 mean)
_R = 6_371_000.0


def haversine_distance(lat1, lon1, lat2, lon2):
    """Great-circle distance in metres between two lat/lon points."""
    la1, lo1 = radians(lat1), radians(lon1)
    la2, lo2 = radians(lat2), radians(lon2)
    dlat = la2 - la1
    dlon = lo2 - lo1
    a = sin(dlat / 2) ** 2 + cos(la1) * cos(la2) * sin(dlon / 2) ** 2
    return _R * 2 * atan2(sqrt(a), sqrt(1 - a))


def bearing(lat1, lon1, lat2, lon2):
    """Initial bearing (0..360 deg, clockwise from north) from point 1 → 2."""
    la1, lo1 = radians(lat1), radians(lon1)
    la2, lo2 = radians(lat2), radians(lon2)
    dlon = lo2 - lo1
    x = sin(dlon) * cos(la2)
    y = cos(la1) * sin(la2) - sin(la1) * cos(la2) * cos(dlon)
    brng = degrees(atan2(x, y))
    return brng % 360.0


def elevation_angle(lat1, lon1, alt1, lat2, lon2, alt2):
    """Elevation angle (degrees, positive = up) from point 1 to point 2.
    Uses flat-earth approximation (fine for distances under ~50 km)."""
    dist = haversine_distance(lat1, lon1, lat2, lon2)
    if dist < 0.1:
        return 0.0
    dalt = alt2 - alt1
    return degrees(atan2(dalt, dist))


def gimbal_angles(rx_lat, rx_lon, rx_alt,
                  tx_lat, tx_lon, tx_alt,
                  heading_offset=0.0):
    """Compute (azimuth_servo, elevation_servo) angles for the gimbal.

    Parameters
    ----------
    rx_*   : receiver (ground station) position
    tx_*   : transmitter (remote) position
    heading_offset : compass bearing the gimbal faces at azimuth=90 deg
                     (i.e. its centre / rest position).  0 means the
                     gimbal at 90 deg points north.

    Returns
    -------
    (az_deg, el_deg) each clamped to 0..180 for servo range.
    """
    brng = bearing(rx_lat, rx_lon, tx_lat, tx_lon)      # 0..360
    elev = elevation_angle(rx_lat, rx_lon, rx_alt,
                           tx_lat, tx_lon, tx_alt)       # may be negative

    # Map absolute bearing to servo frame:
    #   servo 90 deg  ↔  heading_offset bearing
    #   servo 0 deg   ↔  heading_offset - 90  (left limit)
    #   servo 180 deg ↔  heading_offset + 90  (right limit)
    relative = brng - heading_offset          # -360..+360
    # Normalise to -180..+180
    while relative > 180:
        relative -= 360
    while relative < -180:
        relative += 360
    az_servo = relative + 90.0                # shift so 0→90
    az_servo = max(0.0, min(180.0, az_servo))

    # Elevation: 0 = horizontal, 90 = straight up → map to 0..180
    el_servo = elev + 90.0                    # -90→0, 0→90, +90→180
    el_servo = max(0.0, min(180.0, el_servo))

    return az_servo, el_servo
