# Walkthrough: Porting & Kustomisasi kaeru - Redmi 13C (gale)

Panduan ini berisi langkah demi langkah (step-by-step) pengerjaan porting hingga kustomisasi biner bootloader Redmi 13C (gale), yang disusun dengan merujuk langsung ke kedua panduan resmi berikut:
1. 📖 [Porting kaeru to a new device](https://github.com/R0rt1z2/kaeru/wiki/Porting-kaeru-to-a-new-device)
2. 📖 [Customization and kaeru APIs](https://github.com/R0rt1z2/kaeru/wiki/Customization-and-kaeru-APIs)

---

## 1. Perbandingan Alamat Memori (Sebelum vs Sesudah)

Berikut adalah daftar perubahan alamat pada berkas konfigurasi `configs/xiaomi/gale_defconfig` setelah dilakukan analisis statis mendalam pada berkas biner `lk.img`:

| Parameter Config | Sebelum Analisis | Sesudah Analisis | Status / Penjelasan Teknis |
| :--- | :--- | :--- | :--- |
| **`CONFIG_APP_ADDRESS`** | *(Kosong)* | **`0x4C42A3CC`** | **[BARU]** Alamat entry point dari aplikasi booting utama (`app()`). Diperoleh dengan melacak lompatan di akhir penanganan perintah `fastboot continue`. |
| **`CONFIG_BOOTMODE_ADDRESS`** | *(Kosong)* | **`0x4C5765A4`** | **[BARU]** Alamat variabel bootmode di memori RAM/BSS. Ditemukan dari kalkulasi pointer memori di dalam fungsi `fastboot continue`. |
| **`CONFIG_FASTBOOT_FAIL_ADDRESS`** | *(Kosong)* | **`0x4C42B820`** | **[KOREKSI]** Alamat `0x4C42B820` yang awalnya dideteksi sebagai "OKAY" dikoreksi menjadi "FAIL" setelah divalidasi memuat pointer string `"FAIL"`. |
| **`CONFIG_FASTBOOT_OKAY_ADDRESS`** | `0x4C42B820` | **`0x4C42BA00`** | **[KOREKSI]** Diperbarui ke alamat asli fungsi `fastboot_okay()` yang memuat pointer string `"OKAY"`. |
| **`CONFIG_PLATFORM_INIT_CALLER`** | *(Kosong)* | **`0x4C425E0C`** | **[BARU]** Alamat instruksi pemanggil fungsi `platform_init` di thread `bootstrap2`. |

---

## 2. Tahapan Analisis Detil (Porting)

### Langkah 1: Membongkar Biner Bootloader (`lk.img`)
Karena LK MediaTek dikompilasi dalam arsitektur instruksi Thumb-2 (16/32-bit campuran), pembongkaran kode mesin harus disetel khusus agar tidak terjadi salah tafsir kode instruksi. 
Saya menginstal `binutils-arm-none-eabi` dan menjalankan perintah:
```bash
arm-none-eabi-objdump -m arm -M force-thumb -b binary --adjust-vma=0x4C3FFE00 -D lk.img > lk.asm
```
*Catatan: VMA disesuaikan ke `0x4C3FFE00` untuk memperhitungkan header biner MediaTek sebesar 512 byte (0x200).*

### Langkah 2: Memverifikasi Alamat Fastboot Responses
Fungsi `fastboot_okay` dan `fastboot_fail` keduanya melompat ke fungsi pengiriman umum di alamat `0x4C42B5F0`. Saya melacak dua pemanggil fungsi ini:
* Fungsi di alamat `0x4C42B820` memuat literal offset PC yang menunjuk ke alamat `0x4C5182D4`. Di alamat tersebut, data binernya adalah string `"FAIL"`.
* Fungsi di alamat `0x4C42BA00` memuat literal offset PC yang menunjuk ke alamat `0x4C49C964`. Di alamat tersebut, data binernya adalah string `"OKAY"`.

### Langkah 3: Melacak Penunjuk `bootmode` (`CONFIG_BOOTMODE_ADDRESS`)
Di dalam fungsi `fastboot continue` (`0x4C42E1CC`), terdapat instruksi pemuatan variabel bootmode ke register untuk disetel kembali menjadi `BOOTMODE_NORMAL (0)`. 
Saya melacak register penunjuk alamatnya yang dimuat dari alamat literal `0x4C42E228` yang menunjuk ke RAM `0x4C573654`. Di lokasi RAM tersebut, terdapat pointer yang menunjuk ke alamat variabel bootmode sebenarnya, yaitu **`0x4C5765A4`**.

### Langkah 4: Melacak Penunjuk Aplikasi Utama (`CONFIG_APP_ADDRESS`)
Setelah perintah `fastboot continue` dieksekusi, sistem akan melakukan lompatan untuk masuk ke aplikasi utama di alamat:
```assembly
4c42e200:  b.w  0x4c42a3cc
```
Maka alamat `0x4C42A3CC` dikonfirmasi sebagai alamat `app()`.

### Langkah 5: Melacak Platform Init Caller
Saya mencari semua instruksi lompatan `bl` yang menuju ke alamat inisialisasi platform (`0x4C4039DC`) dan menemukan pemanggilnya di thread bootstrap pada alamat **`0x4C425E0C`**:
```assembly
4c425e0c:  bl  0x4c4039dc
```

---

## 3. Kustomisasi & Spoofing (board-gale.c)

Semua kode diletakkan pada berkas `board/xiaomi/board-gale.c`:

* **Bypass Keamanan Gambar (`get_vfy_policy`)**:
  Pola RAM (`0xB508, 0xF7FF, 0xFF63, 0xF3C0`) di alamat `0x4C417B58` dipaksa mengembalikan nilai `0`.
* **Bypass Batasan Flash (`get_dl_policy`)**:
  Pola RAM (`0xB508, 0xF7FF, 0xFF5D, 0xF000`) di alamat `0x4C417B64` dipaksa mengembalikan nilai `0`.
* **Bypass Kesalahan AVB (`avb_slot_verify`)**:
  Pola RAM (`0xF005, 0x0301, 0xF083, 0x0A01, 0x930D, 0x9B70`) di alamat `0x4C465E5A` ditambal dengan `0xF04F, 0x0301`.
* **Spoofing Lock State (`seccfg_get_lock_state` & AVB Cmdline)**:
  1. Memaksa `seccfg_get_lock_state` di alamat `0x4C471120` mengembalikan status `2` (Unlocked) agar bootloader internal berjalan normal.
  2. Menambal AVB cmdline di alamat `0x4C462260` dengan menaruh 1 NOP di alamat offset `+ 0x9C` (`0x4C4622FC`) agar Android mendeteksi status penguncian sebagai `"locked"`.

---

## 4. Build Ulang
```bash
./build.sh gale lk.img
```
