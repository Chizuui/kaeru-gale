# Walkthrough: Porting & Kustomisasi kaeru - Redmi 13C (gale)

Panduan ini berisi langkah demi langkah (step-by-step) pengerjaan porting hingga kustomisasi biner bootloader Redmi 13C (gale), yang disusun dengan merujuk langsung ke kedua panduan resmi berikut:
1. 📖 [Porting kaeru to a new device](https://github.com/R0rt1z2/kaeru/wiki/Porting-kaeru-to-a-new-device)
2. 📖 [Customization and kaeru APIs](https://github.com/R0rt1z2/kaeru/wiki/Customization-and-kaeru-APIs)

---

## 1. Perbandingan Alamat Memori (Sebelum vs Sesudah)

Berikut adalah daftar perubahan alamat pada berkas konfigurasi `configs/xiaomi/gale_defconfig` setelah dilakukan analisis statis mendalam pada berkas biner `lk.img`:

| Parameter Config | Nilai | Status / Penjelasan Teknis |
| :--- | :--- | :--- |
| **`CONFIG_APP_ADDRESS`** | **`0x4C42A3CC`** | **[BARU]** Entry point dari aplikasi booting utama (`app()`). Dilacak dari tail-jump di `fastboot continue`. |
| **`CONFIG_BOOTMODE_ADDRESS`** | **`0x4C5765A4`** | **[BARU]** Variabel bootmode di BSS. Ditemukan dari rantai pointer di fungsi `fastboot continue`. |
| **`CONFIG_FASTBOOT_FAIL_ADDRESS`** | **`0x4C42B820`** | Memuat format string `"FAIL"` (dikonfirmasi via pencarian string). |
| **`CONFIG_FASTBOOT_OKAY_ADDRESS`** | **`0x4C42BA00`** | Memuat format string `"OKAY"`. |
| **`CONFIG_PLATFORM_INIT_CALLER`** | **`0x4C425E0C`** | Instruksi `bl platform_init` di thread `bootstrap2`. |
| **`CONFIG_GET_ENV_ADDRESS`** | **`0x4C45C4E8`** | **[BARU]** Wrapper publik `get_env(char *key)` — dipanggil 25× di LK, tail-call ke `env_lookup_with_area()`. |
| **`CONFIG_SET_ENV_ADDRESS`** | **`0x4C45C700`** | **[BARU]** Wrapper publik `set_env(char *key, char *val)` — tail-call ke `set_env_with_area(key, val, 0)`. |

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

Semua logika kustomisasi diletakkan di `board/xiaomi/board-gale.c`. Alih-alih menerapkan patch awal secara permanen (unconditional), kami memasang hook tepat setelah inisialisasi lingkungan selesai (`env_init_done`) untuk menerapkan patch secara dinamis bergantung pada nilai `is_spoofing_enabled()` (apakah `1` atau `0`).

### Hooking Env Init Done
Karena variabel lingkungan belum siap selama `board_early_init`, kami memasang hook pada tail-call fungsi inisialisasi penyimpanan (`0x4C4057FA`) menggunakan `PATCH_BRANCH` untuk melompat ke `spoof_lock_state_hook()`, yang menjalankan konfigurasi dinamis kami.

### Ketika Spoofing DIAKTIFKAN (`bldr_spoof` bernilai "1")
* **Lock Spoofing (`seccfg_get_lock_state`)**: Menambal badan fungsi di `0x4C471120 + 6` agar menulis `1` (Locked) ke pointer, menyamarkan status penguncian untuk TEE dan OS Android.
* **Spoofing AVB Cmdline**: Menambal NOP pada `beq.n` di `0x4C462260 + 0x9C` — memaksa `verifiedbootstate=green` ke dalam cmdline kernel pada boot normal.
* **Bypass Batasan Fastboot**: Menambal NOP pada gerbang `cbz` dan instruksi `bl fastboot_fail` di pemroses perintah fastboot (`0x4C42B830`) agar perintah fastboot tetap bekerja normal meski status kunci dilaporkan sebagai "locked".
* **Bypass Verifikasi Kunci Chained AVB (`load_and_verify_vbmeta`)**: Menambal NOP pada cabang error path `bne.w` di `0x4C464CF8`, menambal `cmp r2, r3` menjadi `cmp r3, r3` di `0x4C4649CC` (memaksa pengecekan panjang kunci selalu lolos), serta menambal `cmp r3, #0` menjadi `movs r3, #1` di `0x4C464D6A` (memaksa status kunci terpercaya). Ini memungkinkan mem-boot modem/image yang tidak ditandatangani.

### Ketika Spoofing DINONAKTIFKAN (`bldr_spoof` bernilai "0")
* **Lock Spoofing (`seccfg_get_lock_state`)**: Menambal badan fungsi untuk mengembalikan `2` (Unlocked) agar TEE, OS Android, dan fastboot mendeteksi perangkat sebagai tidak terkunci secara penuh.
* **Patch Lainnya**: Dilewati sehingga perangkat berjalan pada kondisi unlocked bawaan yang asli.

---

## 4. Spoofing Cmdline untuk Recovery

Agar TWRP / fastbootd dapat mendeteksi perangkat sebagai **unlocked** saat spoofing aktif, cmdline kernel harus dikembalikan ke status unlocked asli saat booting ke recovery mode.

### Strategi
Memasang hook pada `cmdline_pre_process` melalui `PATCH_CALL`. Jika `get_bootmode() == BOOTMODE_RECOVERY` dan spoofing aktif, `handle_recovery_boot()` akan mengganti:
- `androidboot.verifiedbootstate=green` → `orange`
- `androidboot.secureboot=1` → `0`
- `androidboot.vbmeta.device_state=locked` → `unlocked`

### Alamat Kunci (dari analisis statis)

| Simbol | Alamat | Cara Menemukan |
| :--- | :--- | :--- |
| `cmdline_pre_process` | `0x4C42F544` | Pola `0xE92D, 0x47F0, 0xF7FF, 0xFFA6` di lk.img |
| `g_cmdline` (CMDLINE1) | `0x4C579626` | Decode literal pool PC-relative di cmdline builder |
| Buffer cmdline ke-2 (CMDLINE2) | `0x4C5795D8` | Literal pool berdekatan di fungsi yang sama |

---

## 5. Build Ulang
```bash
./build.sh gale lk.img
```

