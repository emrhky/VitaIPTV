# Vita IPTV

PlayStation Vita (HENkaku/Ensō) için IPTV oynatıcı. Kaynakları `ux0:data/VitaIPTV/sources.txt` dosyasından okur.

## sources.txt biçimi

    # Ad | tür | argümanlar
    Liste 1      | m3u    | http://ornek.com/liste.m3u
    Şifreli liste| m3u    | http://ornek.com/liste.m3u | kullanici | sifre
    Sağlayıcı    | xtream | http://ornek.com:8080 | kullanici | sifre
    Tek kanal    | stream | http://ornek.com/live/1.ts
    Yerel dosya  | file   | ux0:data/VitaIPTV/benim.m3u

- `m3u`: M3U listesi. İsteğe bağlı kullanıcı/şifre HTTP Basic/Digest doğrulaması için kullanılır.
- `xtream`: Xtream Codes tarzı (kullanıcı adı + şifre). Uygulama sağlayıcının JSON API'sini (`player_api.php`) kullanır ve yalnızca canlı kanalları ve kategorileri çeker; film/dizi listeleri çekilmez, bu yüzden büyük panellerde de hızlı ve bellek dostu çalışır. Kategoriler grup olarak görünür.
- `stream`: Tek bir yayın linki, listeye gerek olmadan doğrudan oynatılır.
- `file`: Hafıza kartındaki yerel M3U dosyası.
- Doğrudan linkler (şifresiz) için `m3u` ya da `stream` yeterlidir. Kullanıcı adı/şifre zaten linkin içindeyse (`.../live/kullanici/sifre/1.ts`) ayrıca bir şey girmene gerek yok.

## Kontroller

| Ekran | Tuş |
|---|---|
| Kaynaklar | X aç, Üçgen sources.txt'yi yeniden oku, Start çık |
| Kanallar | X oynat, O geri, L/R grup değiştir, Sol/Sağ sayfa atla |
| Oynatma | O durdur, Yukarı/Aşağı önceki/sonraki kanal, X kanal adını göster |

## Derleme

    export VITASDK=/usr/local/vitasdk   # kendi yolun
    cmake -B build && cmake --build build   # build/VitaIPTV.vpk oluşur

Gereken kütüphane: vita2d. Ağ erişimi için libcurl yerine Vita'nın kendi `sceHttp` kütüphanesi kullanılır.

## PC'de test

    gcc -Wall -Isrc tests/test_host.c src/iptv.c -o test_host && ./test_host

M3U ayrıştırıcı, Xtream adresi ve sources.txt okuyucusu `src/iptv.c` içinde Vita'dan bağımsızdır ve bu testle doğrulanmıştır.

## Bilinen sınırlar

- `src/player.c` Vita üzerinde denenmedi. Görüntü bozuk ya da renkler yanlışsa `player.c` başındaki `FRAME_ALIGN`, `CHROMA_VU`, `DECIMATE` ayarlarına bak.
- Video şimdilik yazılımla RGB'ye çevrilip yarı çözünürlükte çiziliyor (`DECIMATE 2`). GPU ile renk dönüşümü sonraki adım olabilir.
- HLS (.m3u8) ve çeşitli codec'lerin SceAvPlayer'da çalışması yayına bağlıdır. Vita donanımı H.264 + AAC/MP3 çözer; HEVC çözmez.
- Liste indirme sırasında arayüz donar (tek iş parçacığı). Düz `m3u` listeleri 24 MB üstündeyse reddedilir (Xtream için bu sınır geçerli değil).
- Varsayılan yazı tipi Türkçe karakterlerin hepsini göstermeyebilir.
- HTTPS, Vita'nın sistem sertifikalarını kullanır; eski yazılım sürümlerinde bazı HTTPS siteler açılmayabilir, mümkünse `http://` kullan.
- Kullanıcı adı/şifre doğrulaması yalnızca Basic türünü destekler (Xtream için zaten gerekmez).

## Sorun giderme

Oynatıcı her adımı `ux0:data/VitaIPTV/log.txt` dosyasına yazar (kanal adresleri ve şifreler yazılmaz). Uygulama çökerse bu dosyanın son satırı çökmenin hangi adımda olduğunu gösterir. Dosya her açılışta sıfırlanır.

## Yerel video testi

`ux0:data/VitaIPTV/` klasörüne H.264 + AAC bir `.mp4` (ya da `.m4v`, `.mov`, `.ts`) dosyası koyarsan, uygulama açılışında kaynak listesinin sonunda `[Local] dosyaadı` olarak görünür. Seçince doğrudan oynatılır. Sonuç `log.txt` içinde `first video frame` / `first audio frame` satırlarıyla görülür. Oynatıcı kaynağı reddederse ekranda hata kodu gösterilir.

## Ağ üzerinden oynatma (HTTP)

SceAvPlayer'ın kendi okuyucusu `http://` adreslerini kabul etmez. Bu yüzden ağ kaynaklarında uygulama kendi okuyucusunu verir: HTTP Range istekleriyle dosyayı parça parça okur. Okumalar 256 KB'lık bloklarla önbelleğe alınır (oynatıcı MP4 başlıklarını 8 baytlık parçalarla okur). Sunucu Range isteğini desteklemiyorsa (ör. `python -m http.server`), küçük dosyalar (24 MB'a kadar) bir kez belleğe indirilip oradan okunur. Bu yöntem sabit dosyalar (MP4) için çalışır. Canlı yayınlar için ayrı bir çalışma gerekir.

PC testi: `gcc -Wall -Isrc -Itests/mock tests/test_httpio.c src/httpio.c -o test_httpio && ./test_httpio`

## MPEG-TS (canlı IPTV) - 1. adım: akış analizi

SceAvPlayer MPEG-TS, M2TS ve parçalı MP4'ü oynatamıyor (denendi). Canlı kanallar için kendi ayrıştırıcımız var (`src/tsdemux.c`). Şu an bir kanal seçildiğinde (MP4 olmayan her adres ya da `.ts` dosyası) uygulama akışı birkaç saniye okuyup içeriğini ekrana ve `log.txt` dosyasına yazar: kodek, çözünürlük, profil, kare hızı, ses biçimi, kayıp paket sayısı, şifreli mi, Vita donanımıyla oynatılabilir mi. Henüz görüntü oynatmaz.

Ayrıştırıcı testleri (ffmpeg ve ffprobe gerekir):

    python3 tests/run_ts_tests.py
    gcc -Wall -Isrc -Itests/mock tests/test_probe.c src/probe.c src/tsdemux.c -o test_probe

## MPEG-TS (canlı IPTV) - 2. adım: donanım H.264 ile görüntü

MP4 olmayan her kanal artık `src/tsplayer.c` ile oynatılır: akış okunur, `tsdemux` ile ayrıştırılır, H.264 kareler Vita'nın donanım çözücüsüne (`sceAvcdec`) verilir ve RGBA olarak doğrudan GPU dokusuna yazılır. Kareler zaman damgalarına göre gösterilir. Henüz ses yok (3. adım).

Oynatma ekranında: X istatistikleri gösterir, Üçgen akış analizine geçer (analiz ekranında Üçgen tekrar oynatır), Yukarı/Aşağı kanal değiştirir, O geri döner.

Bilinen sınırlar: HEVC ve 10-bit H.264 oynamaz; yayın sırasında çözünürlük değişirse oynatma durur; renkler ters (kırmızı/mavi yer değiştirmiş) görünürse `src/tsplayer.c` başındaki `TEX_FORMAT` değiştirilir.

Test (PC, ffmpeg gerekir): `python3 tests/run_player_tests.py` - gerçek iş parçacıkları ve sahte bir donanım çözücüyle oynatıcıyı sınar.

## MPEG-TS - 3. adım: ses

Ses (AAC) Vita'nın ses çözücüsüyle (`sceAudiodec`) çözülür ve `BGM` portundan çalınır (`MAIN` portu yalnızca 48 kHz kabul eder). Ses ana saattir: görüntü kareleri sesin o an çalan zamanına göre gösterilir. Ses kesilirse görüntü kendi saatine döner. Okuma, görüntü çözme ve ses ayrı iş parçacıklarındadır; aralarında sıkıştırılmış veri kuyrukları vardır.

- Desteklenen ses: AAC (ADTS), 1-2 kanal, 8-48 kHz. AC-3/E-AC-3, MP2 ve çok kanallı AAC'de görüntü oynar, ses kapalı kalır ve nedeni ekranda yazar.
- Görüntüsü olmayan kanallar (radyo) yalnızca ses olarak çalar.
- X ile açılan bilgi satırında ses biçimi ve görüntü-ses farkı (A/V) gösterilir.

## Arayüz ve kontroller (sürüm 4)

| Ekran | Tuşlar |
|---|---|
| Kaynaklar | X aç, Üçgen sources.txt'yi yeniden oku, Start çık |
| Kanallar | X oynat, Kare ara (ekran klavyesi), L/R grup, Sol/Sağ sayfa, O geri |
| Arama sonuçları | X oynat, Kare yeni arama, Select aramayı temizle, O aramadan çık |
| Oynatma | Yukarı/Aşağı kanal, X ayrıntılı bilgi, Üçgen akış analizi, O geri |
| Radyo | Yukarı/Aşağı istasyon, Üçgen analiz, O geri |

- Arama büyük/küçük harf ve Türkçe harf farkını yok sayar ("cocuk" -> "TRT Çocuk"); birden çok kelime yazılabilir, hepsi geçmeli. Arama tüm gruplarda yapılır.
- Ekran kapanması: görüntü oynarken ekran açık kalır; listelerde ve hata ekranlarında sistem ayarına göre kapanabilir. Radyoda ekran kapanabilir ama cihaz uykuya geçmez ve ses devam eder. Güç düğmesiyle uykuya alınırsa çalma durur.
- Radyo kanalları program tablosundan hemen tanınır ve ses seviyesine tepki veren bir gösterge ile oynatılır.
- PC önizleme: `tests/preview/` (FreeType ile arayüzü PNG olarak çizer).

## Sürüm 5: listeler uygulamada, dönüştürme sunucusu, logo

### Listeleri uygulamadan ekleme
- Liste ekranında Kare = ekle, Üçgen = düzenle, Select = sil, START = menü (Ayarlar, Yeniden yükle, Hakkında, Çıkış).
- Türler: Xtream (sunucu + kullanıcı + şifre), M3U bağlantısı, tek yayın bağlantısı. Bir Xtream `get.php?username=...&password=...` bağlantısı yapıştırılırsa sunucu/kullanıcı/şifre otomatik ayrılır ve liste Xtream olarak kaydedilir (yalnız canlı kanallar, hızlı).
- Toplu ekleme hâlâ mümkün: `sources.txt` dosyasını düzenleyin ya da `.m3u` dosyalarını `ux0:data/VitaIPTV/` klasörüne kopyalayın; otomatik listelenir.

### Dönüştürme sunucusu (1080p, HEVC, MKV, MP2/AC-3 ses)
Vita'nın donanımı yalnız H.264 (720p'ye kadar) ve AAC çözebiliyor. Diğer kanallar için evdeki bir bilgisayarda:

    python3 tools/vita_iptv_proxy.py

(Python 3 ve ffmpeg gerekir; Windows'ta `python tools\vita_iptv_proxy.py`.) Program, Vita'ya yazılacak adresi ekrana basar. Vita'da START > Settings > Transcoding server alanına yazın, "Test server" ile deneyin. "Use automatically" açıksa oynatılamayan kanallar (ve sesi desteklenmeyenler) kendiliğinden sunucu üzerinden açılır; oynatırken Kare ile elle de geçilebilir. Sunucu çıktısı: H.264 en fazla 720p ve 30 fps, AAC stereo. 50 fps kanallar da sunucudan akıcı gelir.

### 1080p doğrudan (deneysel)
`reAvPlayer.suprx` (github.com/SonicMastr/ReAvPlayer) `ux0:data/VitaIPTV/` klasörüne konursa uygulama açılışta yükler. Vita'nın çözücüsündeki 720p sınırını kaldırabilir; denenmedi. Sonuç `log.txt` içinde `module ...` ve `sceVideodecInitLibrary 1920x1088` satırlarında görünür.

### Radyo
Yavaş akışlar için okuma 4 KB parçalarla, 15 sn zaman aşımıyla yapılır; bağlantı koparsa otomatik yeniden bağlanılır.

### Logo ve açılış ekranı
`tools/make_assets.py` logo, simge (icon0), LiveArea görselleri ve açılış ekranını üretir (Pillow gerekir). Üretilen dosyalar projede hazırdır.

## Sürüm 6: Türkçe, radyo ekranı kapalıyken, 1080p modülü

### Dil
Konsolun dili Türkçe ise uygulama Türkçe, diğer dillerde İngilizce açılır. START > Ayarlar > Dil ile değiştirilebilir (Otomatik / English / Türkçe). Metinler `src/lang.c` içindedir.

### Radyo ekran kapalıyken
Radyo çalarken uygulama bekleme modunu kilitler; ekran kendiliğinden kararsa da ses devam eder. Radyo ekranında START ekranı hemen kapatır, herhangi bir tuş geri açar. **Güç düğmesine basmayın**: Vita'yı bekleme moduna alır ve homebrew uygulamalar orada durdurulur (bunu yalnızca sistem eklentileri aşabilir).

### 1080p modülü
`reAvPlayer.suprx` dosyasını `ux0:data/VitaIPTV/` klasörüne koyun (taiHEN `config.txt` içine eklemek gerekmez; uygulama kendisi yükler). Şu yerlere de bakılır: `ux0:data/VitaIPTV/modules/`, `ux0:tai/`, `ur0:tai/`, `ux0:data/`, `ux0:`. Durum START > Ayarlar > "1080p modülü" satırında ve `log.txt` içindeki `module ...` satırında görünür. Modül çözücünün 720p sınırını kaldırmazsa 1080p kanallar dönüştürme sunucusuyla oynatılır.

## Sürüm 7

### Kanal ekranı iki panelli
Solda kategoriler (kanal sayılarıyla), sağda seçili kategorinin kanalları. Solda Yukarı/Aşağı kategori seçer, Sağ veya X kanallara geçer; sağda X oynatır, L/R sayfa atlar, Sol veya O kategorilere döner. Kare ile arama tüm kategorilerde yapılır.

### Titreme ve ses kayması düzeltildi
Çözücü artık H.264 seviyesinin gerektirdiği kadar resim belleğiyle açılır (ör. 720p Level 3.2 için 5 kare). Yine yetmezse bir sonraki anahtar karede belleği ikiye katlayarak yeniden açılır. Bellek hataları artık zaman damgalarını kapatmaz (ses-görüntü senkronunu bozan hata buydu).

### Türkçe karakterler
Arayüz artık Vita'nın kendi sistem yazı tipini (PVF) kullanır; ş, ğ, ı, İ gibi harfler görünür. Yazı tipi açılamazsa eski yazı tipine döner.

### 1080p modülü için kurulum
taiHEN `config.txt` içindeki `*main` bölümü ana menüye (LiveArea) yükler, bu uygulamaya değil. Eklentiyi yalnızca bu uygulamaya yüklemek için:

    *VIPTV0001
    ur0:tai/reAvPlayer.suprx

`*main` altındaki satırı kaldırın. Uygulamanın kendisi yüklemeyi denediğinde `0x8002D003` hatası alınıyordu. Modül yüklense bile çözücünün 720p sınırını kaldırıp kaldırmayacağı doğrulanmadı; olmazsa 1080p için dönüştürme sunucusu kullanılır.

## Sürüm 8

### MKV (Matroska / WebM) kanallar
Uygulamanın içinde yazılmış bir MKV ayrıştırıcısı var (`src/mkvdemux.c`): canlı yayınlardaki boyutu bilinmeyen bölümler, paketlenmiş (laced) ses blokları desteklenir. MKV içindeki H.264 + AAC doğrudan donanımda oynar; MKV içinde HEVC, MP3 vb. varsa dönüştürme sunucusu gerekir. `.mkv` / `.webm` dosyaları da yerel dosya olarak listelenir. Test: `python3 tests/run_mkv_tests.py`.

### Radyo
Radyo çalarken ekran video gibi açık kalır. START ekranı bilerek kapatır; herhangi bir tuş geri açar.

### Çözücü belleği
H.264 seviye sınırı + 2 resim (en fazla 10) ile açılır; ilk "bellek yetmedi" hatasında bir sonraki anahtar karede iki katına çıkar. Kanaldan çıkarken bekleyen HTTP isteği iptal edilir (önceden bazen oynatıcı kapanamıyordu).

### ğ harfi
Sistem yazı tipinde ğ/Ğ yok; uygulama bu harfleri g/G üzerine kısa işareti çizerek gösterir.

## Sürüm 9

### 720p kanallar yeniden açılıyor
Sürüm 8'de çözücü 7 referans karesiyle açılmak istendi; Vita'nın kütüphanesi bunu reddetti (`0x80620802`) ve hiçbir video açılmadı. Artık reddedilen sayı birer birer düşürülür, kabul edilen en büyük değer çalışırken büyütmenin de üst sınırı olur. Büyütme mümkün değilse "bellek yetmedi" hatası yalnızca o kareyi atlatır, görüntü donmaz.

### 1080p eklentisi kaldırıldı
reAvPlayer taiHEN ile uygulamaya yüklendiğinde de çözücü 1080p'yi reddetti; bu eklenti bu çözücüye yardım etmiyor. Uygulama artık onu yüklemeye çalışmaz. `config.txt` içindeki `*VIPTV0001` / `ur0:tai/reAvPlayer.suprx` satırlarını silebilirsiniz. 1080p kanallar için dönüştürme sunucusu kullanılır.

### Türkçe harfler
Ekranda ğ, ı, ş, ç, ü, ö (ve büyükleri) g, i, s, c, u, o olarak gösterilir.

### Yetişkin kanallarını gizle
START > Ayarlar > "Yetişkin kanallarını gizle". Açıkken: Xtream sunucusunun yetişkin olarak işaretlediği kanallar (`is_adult`) ve kategori ya da adında "adult", "xxx", "18+", "yetişkin", "erotik" vb. geçen kanallar listelere hiç girmez (aramada da çıkmaz). Bir listeyi açarken uygulanır. Varsayılan: kapalı.

## Sürüm 10: dahili çözücü (1080p, deneysel)

### Neden 1080p ve bazı 720p kanallar açılmıyordu
Log'lar gösterdi ki uygulamalara açık çözücü H.264 Level 3.1 ile sınırlı: resim en fazla 3600 makroblok (720p) ve referans resim belleği en fazla 18000 makroblok (720p'de 5 resim, 960x544'te 8). Level 3.2/4.0 olarak kodlanmış 720p kanallar daha fazla referans resmi istediği için "bellek yetmedi" (0x80620003) hatası alıyor, 1080p hiç başlatılamıyordu.

### Dahili çözücü
ReAvPlayer eklentisinin (github.com/SonicMastr/ReAvPlayer, MIT) kaynağı, 1080p'yi sistemin "Internal" çözücü fonksiyonlarına yönlendirerek açtığını gösteriyor. Uygulama artık bu fonksiyonları eklentisiz, doğrudan kullanmayı deniyor (`src/vdec_internal.c`, taiHEN geçiş kancalarıyla):
- 720p'den büyük yayınlarda doğrudan,
- 720p'de normal çözücünün izin verdiği en fazla resimle bile bellek yetmezse bir sonraki anahtar karede.
START > Ayarlar > "1080p çözücü (deneysel)" ile kapatılabilir. `log.txt` içinde `vdi:` ile başlayan satırlar her adımı gösterir; bir sorun olursa bu satırlar gereklidir. Eklentiyi `config.txt`'ye eklemek gerekmez.

### Diğer
- Çözücü bir hatadan sonra resim vermezse (donma) bir sonraki anahtar karede yeniden açılır.
- İlk bağlantı zaman aşımına uğrarsa iki kez daha denenir; kanaldan çıkarken iptal edilen istek hata olarak gösterilmez.
- START/SELECT düğme resimleri daha okunaklı.
- LiveArea başlatma resmi yalnızca simgedir. Altındaki "Başlat" düğmesi sistemin kendi düğmesidir, kaldırılamaz.

## Sürüm 11: dahili çözücü, ikinci deneme

Sürüm 10'da fonksiyonlar bulundu ama ilk çağrıda (`SetConfigInternal`) uygulama kapandı. Artık:
- Fonksiyonlar kanca (taiHEN hook) yerine doğrudan, SceAvcodecUser'ın dışa aktarım tablosundan alınan adreslerle çağrılır.
- Dört yol sırayla denenir: 0) hazırlıksız, 1) yalnız SetDecodeMode, 2) ReAvPlayer'daki gibi SetConfigInternal + SetDecodeMode, 3) hazırlıksız açıp normal çözme fonksiyonuyla çözme.
- Her yoldan önce `ux0:data/VitaIPTV/vdi_state.txt` dosyasına "trying N" yazılır, ilk resimden sonra "ok N". Uygulama kapanırsa bir sonraki açılışta o yol atlanır. Yani bir kanal uygulamayı kapatırsa, uygulamayı yeniden açıp aynı kanalı tekrar deneyin; her denemede bir sonraki yol kullanılır.
- Bütün yollar kapanmaya yol açarsa dahili çözücü kendiliğinden kapanır. Ayarlar'dan "1080p çözücü" kapatılıp tekrar açılınca kayıt silinir ve her şey baştan denenir.
- Test: `gcc -std=gnu99 -DVDI_TESTING -DSTATE_FILE='"/tmp/vdi_state.txt"' -Isrc -Itests/mock_tai -Itests/mock_rt tests/test_vdi.c src/vdec_internal.c -o test_vdi && ./test_vdi`

## Sürüm 12

### 720p donmaları: referanssız resimleri atlama
Bazı 720p kanallar (çoğu 50 fps) Vita'nın normal çözücüsünün izin verdiğinden (5 resim) fazla resim belleği istiyor. Bu kanallarda bellek hatası sınırda tekrarlanırsa ve dahili çözücü kullanılamıyorsa oynatıcı, başka resimlerin referans almadığı B-resimleri (nal_ref_idc 0) atlar. Çözücüde yeniden sıralanmak için bekleyen ve belleği dolduran resimler bunlardır. Kare hızı kanala göre yarıya kadar düşebilir ama görüntü donmaz. Log'daki `stats` satırında `skipped` sayısı görünür.

### Dahili çözücü: tanı ve yedek yol
Sürüm 11'de dışa aktarım tablosunda fonksiyonlar bulunamadı (sürüm 10'da taiHEN aynı kimlikleri bulmuştu). Artık tablonun içeriği log'a yazılır (`vdi: library ...` satırları) ve tabloda bulunamayan fonksiyonlar taiHEN kancasıyla alınır; denemeler en güvenli yoldan başlar. Dahili çözücü açılamazsa oynatma durmaz: normal çözücüye dönülür ve resim atlama devreye girer.

## Sürüm 13: dahili çözücü kaldırıldı, joystick

### Dahili çözücü geri alındı
Sürüm 10-12'de denenen "dahili çözücü" (sistemin Internal fonksiyonları) yalnızca başarısız olmuyor, cihazda kalıcı zarar veriyordu: bir kez çağrıldıktan sonra normal (720p) kanallar da `0x80620002` hatası verip görüntü üretmez oluyordu (ekranda ses var, görüntü yok). Bu fonksiyonlar bu uygulama türünde güvenle kullanılamıyor. İlgili tüm kod, ayar ve dosyalar kaldırıldı. 1080p ve HEVC için tek güvenilir yol dönüştürme sunucusudur.

Hafıza kartında kalan `ux0:data/VitaIPTV/vdi_state.txt` dosyası artık kullanılmıyor, silebilirsiniz. Eski `settings.txt` içindeki `internal_dec` satırı da yok sayılır.

### 720p donmaları
Normal çözücünün izin verdiğinden (genelde 5 resim) fazla resim belleği isteyen kanallarda (çoğu 50 fps), oynatıcı referanssız B-resimlerini atlayarak devam eder; görüntü donmaz, kare hızı düşebilir. Bu, dahili çözücüden bağımsız çalışır ve korundu.

### Joystick
Kanal ekranında sol analog çubuk kategorilerde, sağ analog çubuk kanallarda yukarı/aşağı gezinir. D-pad eskisi gibi çalışmaya devam eder.

## Sürüm 14: HTTPS, büyük arayüz, 1080p gizleme

- **HTTPS bağlantıları**: `0x80435001` hatası Vita'nın eski sertifika deposundan geliyordu (tinyurl, GitHub, CDN gibi yeni sertifikalar reddediliyordu). Uygulama artık `sceSslInit` çağırıyor ve tüm HTTP istemcilerinde (liste, oynatıcı, analiz, dosya okuyucu) sertifika denetimini kapatıyor (`src/nettls.h`; SDK'da `sceHttpsDisableOption` süreç geneli çalışır). Kısaltılmış (tinyurl) adresler yönlendirmeyle takip edilir. Not: sertifika denetimi kapalı olduğu için https, şifrelemeyi korur ama sunucunun kimliğini doğrulamaz; herkese açık yayın listeleri için bu kabul edilebilir.
- **Büyük üst/alt çubuk**: üst çubuk 60 px, alt çubuk 48 px; yazılar bir kademe büyük, düğme simgeleri (X, O, kare, üçgen) 26 px, START/SELECT hapları daha büyük ve belirgin; üst çubukta uygulama logosu.
- **Ayarlar > 1080p kanalları gizle**: adında/grubunda 1080p, 1080i, FHD, UHD, 4K, 2160p, Full HD geçen kanallar listede görünmez (varsayılan kapalı). `settings.txt` anahtarı: `hide_1080p`.
- **HD kanalda ses var, görüntü yok (`0x80620002`)**: aynı akışlar önceki sürümlerde sorunsuz açılıyordu; hata çözücünün ilk karede parametre reddetmesi. İlk tahmin eklentiydi; log `no reAvPlayer` deyince çürüdü. Gerçek neden: çözücü 720p'de 7, sonra 6 referansla açılmaya çalışılıyordu (en çok 5 kabul edilir); reddedilen denemeler sonraki "başarılı" açılışı bozuyordu. Artık çözünürlüğün sığdırdığından (18000 makroblok / resim) fazlası hiç istenmez ve büyütme de bu sınırı aşmaz. Başlangıçta `ur0:tai/config.txt` taranır ve log'a yazılır (`tai config: ...`); hata ekranı eklentiyi kaldırıp Vita'yı yeniden başlatmayı önerir.

### Sürüm 14, devam
- Çözücü açılıyor ama her kareyi `0x80620002` ile reddediyorsa (log: "decoder refuses every picture"), 2, 3, 4 referansla yeniden açılır. Eski sürümlerde bu yayınlar 2 referansla çalışmıştı.
- Bu hata aynı yayınların daha önce çalıştığı bir cihazda çıkıyorsa kalıcı cihaz durumu olabilir (dahili çözücü denemeleri/çökmeler sonrası): Vita'yı **tamamen kapatıp** (Ayarlar > Güç > Kapat, uyku değil) yeniden açın.
- Liste indirme hataları log'a yazılır (`http: GET https://host failed: ...`). HTTPS el sıkışması başarısızsa (`0x8043....`) uygulama bunu söyler ve `.m3u` dosyasını PC'den `ux0:data/VitaIPTV/` içine kopyalamayı önerir (yerel dosya olarak otomatik eklenir).

### Sürüm 14d: 720p görüntü yok sorununun asıl nedeni
Eski sürümle (VitaIPTV12.zip) karşılaştırınca bulundu: yeni oynatıcı her kare için çözücüden **iki** çıktı resmi istiyordu (`numOfElm = 2`); gerçek Vita bunu `0x80620002` (geçersiz parametre) ile reddediyor, eski sürüm hep tek resim istiyordu. Artık her çağrıda tek resim istenir; ikinci resim yalnızca `0x80620003` (bellek yetmedi) durumunda bir kez denenir ve cihaz bunu da reddederse bir daha denenmez. Test düzeneği "iki resmi reddeden çözücü" durumunu da kapsar. Önceki ek önlemler (referans sayısı sınırı, 2/3/4 ile yeniden açma) yedek olarak durur.

## Sürüm 15: HLS, HTTPS (curl), MP2 ses, kare atlamaları, arayüz

### Kare atlamaları
- Çözücüye iki resim isteyen ikinci deneme, zaman damgalarını kalıcı olarak kapatıyordu (log: `decode with timestamps failed ... retrying without`): kaldırıldı, her çağrı tek resim.
- Bellek yetmeyen (`0x80620003`) tek tük resimler artık yalnızca atlanır. Önceden ilk hatada tüm B (referans olmayan) resimler kalıcı olarak atılıyordu, kare hızı yarıya iniyordu. Şimdi bu yalnızca 100 resimde 10'dan fazla hata olursa ve 30 sn için yapılır.
- Görüntü sese göre sürekli geç kalıyorsa (canlı yayınlarda ses akışta önde gelir; patlamalı gelen resimlerin yarısı atılıyordu) ses bir kez kısa süre durdurulur ve ikisi hizalanır. Log: `video is N ms late; pausing the sound for N ms`.
- Hazır resimler zaman damgasına göre sırayla gösterilir, ekrandakinden eski olan gösterilmez.
- İstatistik satırında yeni bilgiler: `video ±N ms vs audio`, `out of order N`, `screen N fps`.
- Geçmeli (interlaced, 576i TV yayını) H.264 için açık hata mesajı (Vita çözücüsü `0x80620010` ile reddediyor).

### Ses: MP2 / MP3
TV kanallarındaki MPEG-1 Layer II (MP2) ve MP3 ses artık yazılımla çözülür (`src/mpadec.c`, minimp3 - CC0, `src/minimp3.h`). Test: `tests/test_mpadec.c` (ffmpeg'in çözdüğü PCM ile karşılaştırır, MP2'de birebir aynı).

### HLS (.m3u8)
`src/hls.c` (ayrıştırma) + `tsplayer.c` (indirme): ana liste -> en iyi 720p H.264 varyantı -> MPEG-TS parçaları sırayla demuxer'a. Canlı listeler sondan 3 parça geriden başlar ve oynarken yenilenir. Şifreli (AES) ve fMP4 parçalı HLS desteklenmez (açık hata). Testler: `tests/test_hls.c`, `tests/run_player_tests.py` (HLS video, canlı, 404).

### HTTPS: uygulamanın kendi TLS'i (mbedTLS)
Vita'nın kendi TLS'i (`0x80431075`) bugünkü sitelerin çoğuyla el sıkışamıyor. SDK'daki libcurl de SDK'daki OpenSSL ile bağlanamıyor (OpenSSL 1.0.2 fonksiyonları eksik). Bu yüzden `https://` adresleri (liste indirme, yayın, HLS) artık uygulamanın içine gömülü mbedTLS 3.6 (`third_party/mbedtls`, Apache-2.0) ve küçük bir HTTP istemcisiyle (`src/curlio.c`: BSD soketleri, poll ile zaman aşımı, yönlendirme, chunked, basic auth) açılır. Dış paket gerekmez. TLS 1.2 ve 1.3 desteklenir; sertifikalar denetlenmez (Vita'da güncel kök sertifika yok), bağlantı yine şifrelidir. Kapatmak için: `cmake -B build -DVITAIPTV_HTTPS=OFF`. Testler: `python3 tests/run_curlio_test.py` (TLS 1.3 ve yalnız TLS 1.2 sunucu, yönlendirme, 404, auth, durdurma) ve `python3 tests/run_https_tests.py` (oynatıcı HTTPS üzerinden, HLS dahil).

### HTTP 407
407 IPTV sağlayıcısının cevabıdır (Vita'dan değil): genelde hesap başka yerde açık (az önce kapatılan kanal sunucuda henüz kapanmamış) ya da kanal pakette yok. Artık 2 sn arayla iki kez daha denenir; olmazsa açık bir mesaj gösterilir. Bazı sunucular bilinmeyen oynatıcıları reddettiği için uygulama kendini VLC olarak tanıtır (`src/netua.h`).

### Arayüz
- Sağ analog her zaman kanal listesini, sol analog kategorileri kaydırır; mavi vurgu kullanılan çubuğun listesine geçer.
- Oynatma listeleri ekranı: büyük renkli tür etiketleri (XTREAM, M3U, FILE, STREAM, LOCAL), daha büyük adlar ve altında sunucu / dosya adı.
- LiveArea: Başlat kapısındaki logo artık kırpılmıyor (opak, kenar boşluklu); LiveArea arka planı sade, ad sol üstte. Kanal yüklenirken ve listeler yüklenirken markalı bekleme arka planı (`resources/waiting.png`).

## Sürüm 16: log bulguları ve hız

- **Donan görüntü:** bazı kanallarda `0x80620003` hatasından sonra çözücü veri almaya devam edip hiç resim vermiyordu (log: 15 sn'de 9 resim). Artık 40 birim boyunca resim gelmezse çözücü bir sonraki anahtar karede yeniden başlatılır ve sığmayan (referans olmayan) resimler 30 sn atlanır. Log: `no picture from the decoder ...; restarting it`, `decoder restarted`.
- **Sırasız gelen resimler:** bazı HLS kanallarında çözücü resimleri sırasız veriyor (log: `out of order 46`). Bu kanallarda gösterim bir resim daha bekler, böylece önce gelmesi gereken resim atılmaz.
- **HLS kalite yedeği:** seçilen kalite (ör. 720p) 404 verirse sıradaki uygun kalite denenir (en fazla üç). Log metinleri düzeltildi (`HTTP 404` / `connection failed`).
- **Uzun parçalı canlı HLS** (8 sn ve üstü) canlıya daha yakın başlar (sondan 2 parça).
- **407:** yalnız bir kez, 1,5 sn sonra yeniden denenir (sürekli 407 veren kanallarda bekleme kısaldı).
- **Hız:** açılışta işlemci 444 MHz, veri yolu 222, GPU 222, xbar 166 MHz'e çıkarılır (homebrew varsayılanı 333 MHz). TLS, MP2 ses, demux ve ekran çizimi hızlanır. Log: `clocks: ...`.
- Log'da `https: built-in TLS (mbedTLS)` yazar.
