# Megas: kurulum ve ilk kullanım

Megas, sanal bilgisayarın ekranını ve kontrollerini tek pencerede toplar.
Arayüz C ile yazılmıştır; Nuklear düğmeleri çizer. Linux'ta mevcut pencere
sistemi, Windows 11'de Windows'un kendi pencere ve çizim araçları kullanılır.
Rust, tarayıcı, Electron, FFmpeg ve video birleştirme süreci yoktur.

## Kurulum

Tam dağıtım paketi Megas'ı, QEMU'yu, gerekli çalışma kitaplıklarını ve açılış
dosyalarını birlikte taşır. Linux `packaging/install.sh`, Windows
`packaging/install.ps1` uygulamayı yalnızca kullanıcının uygulama klasörüne
kurar. Başka uygulamaların kitaplıklarına, genel PATH'e veya masaüstü temasına
dokunmaz. Kurulum ilerlemesi görünür; kurulum hatasında kopyalanabilir bir
onarım isteği ve kalıcı tanılama kaydının yolu verilir.

Kurulum masaüstü simgesi isteğini sorar. Bir ajan kuruyorsa önce kullanıcıya
sormalı, ardından Linux'ta `--agent --desktop-icon=yes` veya
`--agent --desktop-icon=no`, Windows'ta `-Agent -DesktopIcon Yes` veya
`-Agent -DesktopIcon No` kullanmalıdır. Cevap verilmeden sessizce masaüstü
simgesi eklenmez. Uygulama araması her kurulumda eklenir: Super/Windows
tuşuna basıp **Megas** yazın.

Bu kaynak ağacı henüz yayımlanmış, indirilebilir bir release değildir.
`make bundle` gerçek çalışma bağımlılıklarını toplayan paket üretir.
Yayımlama sırasında `packaging/bundle.py --release-base` ile üretilecek
tek kurulum dosyası paketi HTTPS ile indirir, derleme sırasında sabitlenen
SHA256 değerini doğrular ve kurulumu tamamlar. Kullanıcıya paket yöneticisi
komutları veya derleyici kurma görevi verilmez. İndirme veya doğrulama
başarısızsa kısmi paket çalıştırılmaz.

## Pencereyi açtıktan sonra

1. VM'in diskini veya kurulum ortamını seçin.
2. VM'e ayrılacak belleği ve başlangıç ekran boyutunu seçin.
3. **Başlat** düğmesine basın. Gerçek işlem kimliği ve ilerleme görünür.
4. Ekrana tıklayarak fareyi; ekran odaktayken klavyeyi kullanın.
5. Geçmişteki bir kayıtlı duruma dönmek için geçmiş listesinden seçim yapın.

Ekran, QEMU'nun sürekli açık ekran bağlantısından gelir. Ekran görüntüsü
düğmesi aynı canlı tamponun o andaki PNG kopyasını kaydeder. Ajanın her
kontrol yanıtına güncel durum, son resmin yolu, resmin yaşı ve okunabilir
durum metni eklenir. Ajan bağlama yöntemi [harnesses.md](harnesses.md)
dosyasındadır.

Çalışırken gerçek ekran çözünürlüğü değişikliği, misafirin ekran sürücüsünün
bu isteği desteklemesini gerektirir. Bir pencereyi büyütmek, misafirin gerçek
çözünürlüğünü değiştirmek değildir. Destek bulunmadığında Megas değişiklik
yapılmış gibi göstermez; gözlenen ve istenen boyutlar ayrı gösterilir.

## Şifresiz VM'ler

Megas'ın kontrol bağlantısı VM hesabının şifresinden ayrıdır. Yeni bir boş
diskte henüz işletim sistemi veya kullanıcı hesabı bulunmaz. Mevcut bir disk
de kendi giriş ve ekran kilidi ayarlarını taşır.

Şifresiz giriş ve yönetici yetkisi ayrı ayarlardır. Yeni desteklenen Linux
kurulumunda **NoCloud seed**, başlangıçta seçilen kullanıcının kendi VM'i
için `sudo` parolasız yönetici komutlarını hazırlar; bu, masaüstü girişini
otomatik yapmaz. Var olan bir misafirdeki **Şifresiz oturum** işlemi ise
LightDM, SDDM veya GDM otomatik girişini, GNOME ekran kilidini ve yalnızca o
hesaba ait yönetilen `sudoers` kuralını ayarlar. `guest/linux-policy.sh`
önceki yerel hesap ve giriş ayarlarını bir kez saklar. Sonradan parola
girildiğinde parola stdin üzerinden misafire aktarılır; günlük veya işlem
argümanlarına yazılmaz. **Geri yükle** önceki hesap ve giriş ayarlarını
getirir.

`guest/windows-policy.ps1`, Windows içinde ayrı yerel kullanıcıyı hazırlar.
Megas, açıkça girilen parolayı QEMU Guest Agent üzerinden stdin ile aktarır;
yardımcı yerel hesabı oluşturup parolayı guest içinde ayarlar, sonra kaydedilmiş
oturum ve kilit ayarlarını geri getirir. Yardımcı ayrıca QEMU Guest Agent
parolayı önceden ayarlamışsa `-PasswordAlreadySet` seçeneğini destekler; normal
Megas akışı bu yolu kullanmaz. Önceki Winlogon ve ekran kilidi değerleri
korumalı bir guest klasöründe saklanır. Windows önceki parolayı dışa aktaramaz;
şifresiz hesap geri yüklendikten sonra kullanıcıya guest içinde yeni parola
belirlemesi söylenir.

Bu değişiklikler **yalnızca QEMU içindeki hedef misafire** uygulanır. Yardımcılar
`MEGAS-GUEST` kimliğini ve yönetici yetkisini doğrular; QEMU Guest Agent
bağlantısı olmadan değişiklik yapmaz. Misafir işletim sistemi veya ekran kilidi
politikası bu ayarı reddedebilir; Megas doğrulanmamış sonucu başarılı diye
göstermez. Yönetilen/domain politikaları ve başka kilit ekranı yazılımları
yerel ayarları geçersiz kılabilir.

## Doom'u sanal bilgisayarda açma

**Doom** oyunu ev sahibi bilgisayara değil, Megas penceresindeki Linux misafire
kurulur ve orada çalışır. Misafirde QEMU Guest Agent etkin olmalı, seçilen
yerel masaüstü kullanıcısının oturumu açık olmalı ve canlı ekran Megas'a
bağlanmalıdır. Debian/Ubuntu misafirinde Megas oyun motoru **Chocolate Doom**
ve ücretsiz **Freedoom** oyun dosyalarını, misafirin kendi imzalı apt
depolarından kurabilir. Bu işlem ilerleme, gerçek süreç kimliği ve misafir içi
günlükle izlenir; ev sahibi paketlerine dokunmaz.

Mevcut bir oyun dosyası kullanılıyorsa WAD yolu misafirin içindeki mutlak yol
olmalıdır. Ticari Doom oyun verisi indirilmez veya Megas paketine eklenmez.
Başarılı başlatmada oyun VM'in canlı ekranında görünür; masaüstü oturumu veya
WAD yolu desteklenmiyorsa Megas onarım isteği ve misafir günlüklerini gösterir.

## Kaynak ve hız bütçesi

Bellek bütçesi kontrol arayüzü içindir. VM'e ayrılan RAM ayrı bir
kullanıcı seçimidir; 512 MB RAM verilen bir VM ile bütün süreçlerin toplamını
100 MB altında tutmak mümkün değildir. Ekran boyutu ve VM kaynakları arttıkça
maliyet artar. Ölçülmüş değerler [validation.md](validation.md) içindedir.

Kayıtlı durum değiştirme için hedef 5 saniyedir. İşlem sırasında bu hedef
ve ölçülen süre görünür. Büyük VM belleği, yavaş disk veya işletim sistemi
baskısı altında genel bir 5 saniye garantisi yoktur. Önceki kayıtlı durumlar
varsa ileri/geri geçiş tekrar komut oynatmadan yapılır.

Checkpoint alanı, yönetilen qcow2 diskin QEMU tarafından bildirilen ayrılmış
`actual-size` büyümesine göre sınırlandırılır. Bütçe şu formülle belirlenir:
`max(2 GiB, 2 × yapılandırılmış VM RAM'i + 64 MiB)`. Yeni bir durum kaydedilirken
bu bütçede yapılandırılmış VM RAM'i için yer ayrılır; ayrıca diskin bulunduğu
dosya sisteminde en az `512 MiB + yapılandırılmış VM RAM'i` boş alan tutulur.
Baskı oluşursa en eski Megas kayıtlı durumları sırayla silinip alan yeniden
ölçülür; yer açılamazsa yeni durum başarısız bildirilir.

QMP'deki Megas kayıtlı durum etiketleri dahili `megas-owned:` önekiyle
saklanır; kullanıcıya gösterilen adlarda bu önek görünmez. Megas en fazla 16
kendi durumunu saklar ve toplam katalog 64 kayıtla sınırlıdır. Önceki bir
Megas denetleyicisinin durumları yeniden açılışta içe aktarılır. Yabancı
durumlar otomatik silinmez; QEMU kayıtlı VM belleği bulunduğunu doğruluyorsa
kullanıcı bunları açıkça geri yükleyebilir. Örneğin `megas-owned-important`
geçerli eski bir addır ve yabancı durum olarak kalır. Yalnızca disk durumu
içeren kayıtlar listelenebilir ancak VM'in tam RAM ve aygıt durumunu geri
yükleyemez. Günlük, kayıtlı durumların dışında korunur.

## Geliştirici için derleme

Bu bölüm son kullanıcı kurulum akışı değildir. Tam QEMU ağacını derlemeden
kontrol arayüzü ayrı derlenebilir:

```sh
make -C tools/megas all headless
make -C tools/megas test
make -C tools/megas bundle
```

Belirli bir QEMU derlemesini paketlemek için eşleşen çalışma dosyalarının
yollarını verin. Bu yalnızca derleme çıktısını değiştirir:

```sh
make -C tools/megas bundle \
  QEMU=/absolute/path/to/qemu-system-x86_64 \
  QEMU_IMG=/absolute/path/to/qemu-img \
  QEMU_DATA=/absolute/path/to/share/qemu \
  QEMU_MODULES=/absolute/path/to/lib/qemu
```

Derleme makinesinde Tesseract ve `eng.traineddata` varsa `make bundle` bunları
özel `ocr/` klasörüne ve gereken kütüphaneleri pakete ekler. GitHub Actions
derleme runner'larına Tesseract ile İngilizce verisini ayrıca kurar; böylece
üretilen iki platform paketi de özel OCR çalışma dosyalarını taşır. `TESSERACT`
ve `TESSERACT_DATA` ile başka bir yerel kurulum seçilebilir. OCR'ın bellek
kullanımı çalışma sırasında ölçülür ve 48 MiB sınırı aşılırsa işlem durdurulur.

Linux derleme ortamında C derleyicisi ve X11 başlıkları; Windows için
MinGW-w64 C derleyicisi yeterlidir. Test ve paket üretimi derleme makinesinde
Python standard library kullanır; Python uygulamanın çalışma bağımlılığı
değildir. Gerçek VM testleri ve tam paket üretimi mevcut QEMU ile qemu-img
gerektirir. GitHub Actions dosyası derleme makinesi gereksinimlerini kendisi
hazırlar ve Linux/Windows paketlerini artifact olarak saklar. Varsayılan paket
üretimi derleme runner'ında bulunan QEMU'yu ve ona eşleşen firmware/modülleri
dışa aktarır; CI bundle kontrolü bu kaynak ağacının QEMU motorunu derlediği
anlamına gelmez. `licenses/runtime-source.txt` paketteki QEMU, `qemu-img` ve
varsa Tesseract sürümünü, kaynağını ve eksik isteğe bağlı modülleri belirtir.
Kendi fork derlemeni paketlemek için `make bundle` komutuna
eşleşen QEMU, `qemu-img`, firmware ve modül yollarını açıkça ver. Windows işi
gerçek bir Windows runner üzerinde derler ve test eder; Windows 11 masaüstünde
tam kurulum doğrulaması yapmaz.

## Kısa sözlük

| Görülebilecek kavram | Ne işe yarar, kimin için? |
| --- | --- |
| VM / sanal bilgisayar | İçinde ayrı işletim sistemi açılan bilgisayar. Kullanıcının çalışma alanı. |
| Misafir / guest | VM'in içindeki işletim sistemi. Ekran kilidi ve giriş hesabı burada bulunur. |
| Ev sahibi / host | Megas'ı çalıştıran gerçek bilgisayar. VM'e RAM ve işlem gücü sağlar. |
| QEMU | Sanal bilgisayarı çalıştıran motor. Kullanıcı komut ezberlemez; Megas yönetir. |
| Nuklear | Düğme, metin alanı ve listeleri çizen küçük C arayüz kitaplığı. Geliştirici içindir. |
| RFB / VNC | Ekran değişikliklerini ve giriş olaylarını açık bağlantıda taşıyan protokol. Canlı ekran için kullanılır. |
| QMP | Motorun çalışma durumunu doğrulayan kontrol bağlantısı. Kullanıcının elle konuşması gerekmez. |
| qcow2 | Kaydedilmiş VM durumlarını taşıyabilen disk biçimi. Hızlı geçmiş dönüşü isteyenler içindir. |
| Checkpoint / kayıtlı durum | VM belleği ve diskinin o anki geri dönülebilir noktası. Bir günlük satırıyla aynı değildir. |
| Harness | Claude Code, Codex, OpenCode veya Grok gibi ajanı çalıştıran uygulama. |
| Hook | Ajan uygulamasının belirli olaylarda otomatik çağırdığı küçük bağlantı. Güncel durumu bağlama ekler. |
| OCR | Ekrandaki yazıyı Tesseract ile okuma. Görselden ayrı bir sonuçtur; belleği ve süresi sınırlıdır. |
| PNG / ekran görüntüsü | Paylaşılabilir resim dosyası. Canlı ekranın yerine kullanılmaz. |
| SHA256 | İndirilen paketin beklenen dosyayla aynı olduğunu kontrol eden özet. Kurulum otomatik doğrular. |
