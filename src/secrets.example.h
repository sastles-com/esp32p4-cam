// secrets.h にコピーして値を埋める (secrets.h は .gitignore 済み)
#pragma once

#define WIFI_SSID     "your-ssid"
#define WIFI_PASSWORD "your-password"

#define MQTT_BROKER_URI "mqtt://192.168.1.10:1883"
#define CAMERA_ID       "p4cam01"

// 撮影画像のアップロード先 (PC 上の FastAPI)
#define UPLOAD_URL "http://192.168.1.10:8000/upload"
