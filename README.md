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
