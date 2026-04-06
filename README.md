# Sistem Regulasi Nutrisi Adaptif pada Aeroponik Kentang Berbasis ESP32

## Deskripsi

Proyek ini merupakan implementasi sistem kontrol otomatis untuk mengatur nutrisi pada tanaman kentang dengan metode aeroponik. Sistem menggunakan mikrokontroler ESP32 untuk membaca parameter lingkungan dan mengontrol pemberian nutrisi secara adaptif.

## Tujuan

* Menjaga kestabilan pH dan Electrical Conductivity (EC)
* Mengoptimalkan pertumbuhan tanaman kentang aeroponik
* Mengotomatisasi proses pemberian nutrisi dan penyemprotan

## Fitur Utama

* Monitoring pH dan EC secara real-time
* Monitoring suhu air, suhu udara, dan kelembapan
* Kontrol otomatis nutrisi (AB mix)
* Kontrol pH Up dan pH Down
* Sistem chamber sampling untuk pembacaan sensor lebih stabil
* Mode manual dan otomatis
* Penyemprotan (spray) otomatis berbasis waktu
* Penyimpanan data setpoint menggunakan EEPROM
* Tampilan LCD 20x4
* Integrasi Firebase (IoT monitoring)

## Perangkat Keras

### Sensor:

* Sensor pH
* Sensor EC
* DS18B20 (suhu air)
* DHT22 (suhu & kelembapan udara)
* BH1750 (intensitas cahaya)

### Aktuator:

* Pompa nutrisi A dan B
* Pompa mixer
* Pompa sampling
* Pompa buang
* Pompa pH Up
* Pompa pH Down
* Pompa spray
* Lampu

## Library yang Digunakan

* Wire
* LiquidCrystal_I2C
* BH1750
* EEPROM
* OneWire
* DallasTemperature
* DHT
* RTClib
* Firebase_ESP_Client

## Cara Instalasi

1. Install Arduino IDE
2. Tambahkan board ESP32 melalui Board Manager
3. Install semua library yang dibutuhkan
4. Upload program ke ESP32

## Konfigurasi

Sebelum menjalankan program, ubah konfigurasi berikut:

```cpp
#define WIFI_SSID "NAMA_WIFI"
#define WIFI_PASSWORD "PASSWORD_WIFI"
#define API_KEY "API_KEY_FIREBASE"
#define DATABASE_URL "URL_FIREBASE"
```

## Cara Penggunaan

1. Nyalakan sistem
2. Lakukan kalibrasi sensor pH dan EC
3. Atur setpoint melalui menu LCD
4. Pilih mode otomatis atau manual
5. Sistem akan bekerja sesuai parameter yang ditentukan

## Struktur Program

Program terdiri dari beberapa bagian utama:

* Inisialisasi sensor dan aktuator
* Pembacaan sensor
* Sistem kontrol nutrisi
* Sistem sampling
* Sistem spray otomatis
* Integrasi Firebase
* Penyimpanan EEPROM

## Catatan

* Kredensial WiFi dan Firebase tidak disertakan dalam repositori ini demi keamanan
* Pastikan semua sensor telah dikalibrasi sebelum digunakan

## Penulis

Rizki Anugrah Putra
Teknik Elektro
Universitas Jenderal Soedirman

## Tahun
2025
