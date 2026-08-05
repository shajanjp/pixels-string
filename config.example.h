// ============================================================
// WiFi & Network Configuration (example template)
// ============================================================
// Copy this file to config.h and fill in your actual values.
// config.h is gitignored to keep secrets out of version control.

#ifndef CONFIG_H
#define CONFIG_H

// --- WiFi Credentials ---
#define WIFI_SSID       "your-ssid"
#define WIFI_PASSWORD   "your-password"

// --- Static IP Configuration ---
#define STATIC_IP       IPAddress(192, 168, 1, 55)
#define GATEWAY         IPAddress(192, 168, 1, 1)
#define SUBNET          IPAddress(255, 255, 255, 0)

// --- mDNS ---
#define MDNS_HOSTNAME   "pixels-string"



#endif // CONFIG_H
