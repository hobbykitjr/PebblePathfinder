// Pathfinder — GPS + location management

var settings = {
  unit: 0,      // 0=mi, 1=km
  theme: 0,     // 0=classic, 1=minimal, 2=tech
  poll: 2,      // 0=manual, 1=low, 2=med, 3=high
  locations: [] // [{name, lat, lon}, ...]
};

// ============================================================================
// GPS
// ============================================================================
function sendGPS() {
  navigator.geolocation.getCurrentPosition(
    function(pos) {
      var lat = Math.round(pos.coords.latitude * 10000);
      var lon = Math.round(pos.coords.longitude * 10000);
      console.log('GPS: ' + lat/10000 + ', ' + lon/10000);
      Pebble.sendAppMessage({'GPS_LAT': lat, 'GPS_LON': lon},
        function() { console.log('GPS sent'); },
        function() { console.log('GPS send failed'); });
    },
    function(err) { console.log('GPS error: ' + err.message); },
    {timeout: 15000, maximumAge: 30000, enableHighAccuracy: true}
  );
}

// ============================================================================
// GEOCODING
// ============================================================================
function geocodeAddress(address, callback) {
  var url = 'https://geocoding-api.open-meteo.com/v1/search?name=' +
            encodeURIComponent(address) + '&count=1&language=en&format=json';
  var xhr = new XMLHttpRequest();
  xhr.onload = function() {
    if(xhr.status === 200) {
      try {
        var j = JSON.parse(xhr.responseText);
        if(j.results && j.results.length > 0) {
          callback(j.results[0].latitude, j.results[0].longitude);
        } else {
          callback(null, null);
        }
      } catch(e) { callback(null, null); }
    }
  };
  xhr.onerror = function() { callback(null, null); };
  xhr.open('GET', url);
  xhr.send();
}

// ============================================================================
// SEND CONFIG TO WATCH
// ============================================================================
function sendConfig() {
  var packed = settings.unit | (settings.theme << 2) | (settings.poll << 4);
  var msg = {
    'SETTINGS': packed,
    'LOC_COUNT': settings.locations.length
  };

  // Add each location
  var latKeys = ['LOC1_LAT','LOC2_LAT','LOC3_LAT','LOC4_LAT','LOC5_LAT','LOC6_LAT'];
  var lonKeys = ['LOC1_LON','LOC2_LON','LOC3_LON','LOC4_LON','LOC5_LON','LOC6_LON'];
  var nameKeys = ['LOC1_NAME','LOC2_NAME','LOC3_NAME','LOC4_NAME','LOC5_NAME','LOC6_NAME'];

  for(var i=0; i<settings.locations.length && i<6; i++) {
    var loc = settings.locations[i];
    msg[latKeys[i]] = Math.round(loc.lat * 10000);
    msg[lonKeys[i]] = Math.round(loc.lon * 10000);
    msg[nameKeys[i]] = loc.name.substring(0, 19);
  }

  Pebble.sendAppMessage(msg,
    function() { console.log('Config sent'); },
    function() { console.log('Config send failed'); });
}

// ============================================================================
// SETTINGS
// ============================================================================
function loadSettings() {
  try {
    var s = localStorage.getItem('settings');
    if(s) settings = JSON.parse(s);
  } catch(e) {}
}

function saveSettings() {
  localStorage.setItem('settings', JSON.stringify(settings));
}

// ============================================================================
// CONFIG PAGE
// ============================================================================
Pebble.addEventListener('showConfiguration', function() {
  var url = 'https://hobbykitjr.github.io/PebblePathfinder/config/index.html' +
    '?settings=' + encodeURIComponent(JSON.stringify(settings));
  console.log('Opening config');
  Pebble.openURL(url);
});

Pebble.addEventListener('webviewclosed', function(e) {
  if(e && e.response && e.response.length > 0) {
    try {
      var raw = e.response;
      if(raw.indexOf('response=') === 0) raw = raw.substring(9);
      var config = JSON.parse(decodeURIComponent(raw));

      if(config.unit !== undefined) settings.unit = parseInt(config.unit);
      if(config.theme !== undefined) settings.theme = parseInt(config.theme);
      if(config.poll !== undefined) settings.poll = parseInt(config.poll);
      if(config.locations) settings.locations = config.locations;

      saveSettings();
      sendConfig();
      sendGPS();
    } catch(err) {
      console.log('Config parse error: ' + err);
    }
  }
});

// ============================================================================
// EVENTS
// ============================================================================
Pebble.addEventListener('ready', function() {
  console.log('Pathfinder JS ready');
  loadSettings();
  sendConfig();
  sendGPS();
});

Pebble.addEventListener('appmessage', function(e) {
  if(e.payload['REQUEST_GPS']) sendGPS();
});
