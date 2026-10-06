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
