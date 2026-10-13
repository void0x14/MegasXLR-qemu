# Paket doğrulama kaydı

Bu kayıt kaynak ağacında bakım ve inceleme içindir; son kullanıcı paketine
kopyalanmaz. Doğrulama paketi yerel test çıktısıdır, yayımlanmış bir release
değildir.

| Kontrol | Sonuç |
| --- | --- |
| Paket | `build/packaging-final/bundle.tar.gz` |
| SHA256 | `b01dd61dcda49b0fcb634feedbc5edadb14ff1fab4b120b4ecfb545a9a39e5bf` |
| Paket / açılmış boyut | 55 MiB / yaklaşık 150 MiB |
| Dahil edilen QEMU ve qemu-img | 8.2.2, test runner'ından alınan Debian paketi |
| Dahil edilen OCR | Tesseract 5.3.4, İngilizce `eng.traineddata`, özel runtime 70.0 MiB |
| QEMU kaynak durumu | Runner runtime'ı içe aktarılmış; bu paket adımı fork QEMU'yu derlemiyor |

Tam uygulama paketinden, kurulum dizini dışındaki çalışma klasöründen yapılan
15 canlı QEMU sözleşme kontrolünün tamamı geçti (8.03 sn): QMP/RFB bağlantısı,
ekran kareleri ve PNG, klavye/fare girdisi, başlatma/durdurma,
duraklatma/devam, anlık görüntü kaydı ve geri yükleme. Dört geri yükleme
344–650 ms sürdü. Paket içindeki Tesseract canlı ekran metnini okudu; ölçülen
tepe bellek kullanımı 38,633,472 bayt (36.84 MiB), 48 MiB sınırının altında.

Paketin SHA256 manifesti doğrulandı; geçici kullanıcı kurulumunda uygulama
menüsü oluşturuldu ve `--desktop-icon=no` seçimi uygulandı. Uygulama, QEMU,
`qemu-img` ve OCR ikilileri paketin özel kopyalarından açıldı. Çalışma ortamı
QEMU 8.2.2 test girdisiyle eşleşir; fork'un daha yeni QEMU motoru için aynı
paket doğrulaması yapılmış sayılmaz. İsteğe bağlı `chardev-baum.so`, test
runner'ında eksik `libbrlapi.so.0.8` nedeniyle pakete alınmadı; sebep
`licenses/runtime-source.txt` içinde kaydedildi.
