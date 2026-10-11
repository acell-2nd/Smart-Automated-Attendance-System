"""
Mengubah log Serial Monitor menjadi file CSV (bisa dibuka di Excel).

Cara pakai:
  1. Salin seluruh isi Serial Monitor ke file teks, misalnya log.txt
  2. python ambil_csv.py log.txt
  3. Hasil: data_absensi.csv  (kolom: uid, nama, tanggal, jam_scan)
"""
import csv
import sys

if len(sys.argv) < 2:
    print("Pemakaian: python ambil_csv.py log.txt")
    sys.exit(1)

baris_csv = []
with open(sys.argv[1], encoding="utf-8", errors="ignore") as f:
    for baris in f:
        pos = baris.find("CSV,")
        if pos >= 0:
            kolom = baris[pos + 4:].strip().split(",")
            if len(kolom) == 4:
                baris_csv.append(kolom)

if not baris_csv:
    print("Tidak ada baris data ditemukan. Pastikan sudah ada absen berhasil.")
    sys.exit(1)

with open("data_absensi.csv", "w", newline="", encoding="utf-8") as f:
    w = csv.writer(f)
    w.writerow(["uid", "nama", "tanggal", "jam_scan"])
    w.writerows(baris_csv)

print(f"Tersimpan: data_absensi.csv ({len(baris_csv)} baris)")
