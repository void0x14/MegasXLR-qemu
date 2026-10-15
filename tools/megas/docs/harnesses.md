# Ajan bağlantısı

Megas VM'i tek bir yerel süreçte yönetir. Ajanlar yeni QEMU süreçleri
açmak yerine bu sürece komut verir. Her yanıt güncel durum, görsel ve
okunabilir durum metni taşır; komut başarılı görünse bile ekran ve motor
durumu ayrıca doğrulanır. Her talebi ayrıntılı günlükte izleyebilirsiniz.

```sh
megas gui
megas status
megas send '{"action":"key","key":"Return"}'
megas agent
```

`agent` native C içinde çalışan stdio araç bağlantısıdır. Destekleyen
harness'e bağlandığında araç sonuçlarında metin ve PNG resim bloğu birlikte
verilir. Görsel desteği olmayan bağlantı için `megas agent --text-only`
kullanılır. Ek bir Python, Node veya Rust sunucusu açılmaz. Ayrı açık GUI
veya `serve` sahibi gerekir; port ve çalışma klasörü ikisinde aynı olmalıdır.

Metin durumu motorun doğruladığı çalışma bilgilerini anlatır. `read_screen`
eylemi güncel ekran için ayrı bir Tesseract OCR sonucu ister; çıktı OCR olarak
etiketlenir ve bellek, süre ve metin boyutu sınırlarına tabidir. OCR
kullanılamıyorsa Megas resimdeki yazıyı tahmin edilmiş metin diye sunmaz.
Genel amaçlı harness'ler kesintisiz video algısı veya her karede düşünme
sağlamaz. Sürekli ekran bağlantısı açık kalır; model resmi desteklediği araç
sonucunda alır. Bu sınırların üzerindeki destek, ilgili harness'in giriş
katmanını değiştirmeyi gerektirir.

## Otomatik durum ekleme

`megas hook EVENT` güncel durumu harness'in `additionalContext` biçiminde
çıkarır. `packaging/hooks/claude.json` ve `codex.json` başlangıç, yeni kullanıcı
isteği, araç öncesi ve araç sonrası olayları kapsar. `@MEGAS@` yer tutucusu
kurulu native uygulamanın mutlak yolu ile değiştirilmelidir. Özel port veya
klasör varsa `--runtime` ve `--port` seçenekleri `hook` önüne eklenir.

| Uygulama | Entegrasyon ve gerçek kapsam |
| --- | --- |
| Claude Code | Proje `.claude/settings.json` dosyasına mevcut ayarları koruyarak Megas `hooks` grupları eklenir. Önce/sonra durum metni model bağlamına girer. Native araç bağlantısı görsel sonuç verir. |
| Codex CLI | Proje `.codex/hooks.json` dosyasında mevcut gruplara eklenir. Codex yeni/değişmiş hook tanımını kendi güven incelemesinden sonra çalıştırır. `/hooks` ekranındaki onay Megas tarafından atlanmaz. Yerel araç çağrıları kapsanır; hosted araçların tamamı kapsanmaz. |
| OpenCode | `packaging/hooks/opencode.js` proje `.opencode/plugins/` içine konur. `MEGAS_BIN`, isteğe bağlı `MEGAS_RUNTIME` ve `MEGAS_PORT` seçilir. V1 plugin sözleşmesinde araç sonrası sonuca durum ekler, compaction sırasında korur. OpenCode'un zaten kullandığı Bun içinde çalışır; npm paketi indirmez. |
| Grok Build | Resmî `grok -p` ve `grok agent stdio` arayüzleri bulunur. GUI onarım düğmesi `grok -p` ile isteği gönderip süreç ve çıktıyı gösterir. Bu değişiklik Grok'un her araç olayında bağlama enjeksiyon yaptığını iddia etmez; doğrulanmamış hook şeması üretilmez. |

Mevcut yapılandırmalar otomatik silinmez veya bütünüyle değiştirilmez. Proje
ayarındaki port/klasör ile açık VM sahibi eşleşmezse bağlamda açıkça
ulaşılamayan durum ve onarım isteği görünür. Ajan hook'un çalışıp çalışmadığını
gerçek çağrı ile kontrol etmelidir; örnek JSON dosyası olması kurulumun
tamamlandığı anlamına gelmez.

## Onarım düğmeleri

GUI'deki ajan seçimi mevcut yerel yürütülebilir dosyayı bulup tek bir
onarım isteği ile başlatır. Başlatılan işlem kimliği ve çıktı günlüğü gösterilir;
aynı ajan için ikinci gizli süreç açılmaz. Hesap girişi ve harness'in kendi
yetkileri korunur. Harness yoksa veya çağrı sözleşmesi desteklenmiyorsa
kopyalanabilir onarım isteği gösterilir. Bu düğmeler harness kurulumunu,
oturum açmayı veya ücretli hesap erişimini kendiliğinden tamamlamaz.

Pano kullanılabiliyorsa istek panoya kopyalanır. Pano yoksa istek görünür
metin ve kaydedilebilir metin dosyası olarak kullanılabilir; VM kontrolü bu
sebeple durmaz.

## Doğrulanan birincil kaynaklar

Kaynaklar 13 Ekim 2026 tarihinde kontrol edildi; kurulu sürüm farklıysa
entegrasyon o sürüm üzerinde yeniden doğrulanmalıdır.

- [Claude Code hook sözleşmesi](https://code.claude.com/docs/en/hooks)
- [Claude Code native CLI](https://code.claude.com/docs/en/cli-reference)
- [Codex hook ve araç kapsamı](https://developers.openai.com/codex/hooks)
- [Codex native CLI](https://developers.openai.com/codex/cli/reference)
- [OpenCode yerel plugin olayları](https://docs.opencode.ai/docs/plugins/)
- [OpenCode CLI](https://docs.opencode.ai/docs/cli/)
- [Grok Build headless ve ACP](https://docs.x.ai/build/cli/headless-scripting)
- [Grok Build CLI](https://docs.x.ai/build/cli/reference)

Bu değişiklikte gerçek model/harness oturumları için kimlik doğrulama
yapılmadı. `test_agent.py` native stdio sözleşmesini, metin ve görsel
yanıtlarını gerçek Megas sahibiyle test eder; sağlayıcıların kurulu
harness sürümleri üzerinde canlı uçtan uca testin yerine geçmez.
