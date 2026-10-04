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
- `xtream`: Xtream Codes tarzı (kullanıcı adı + şifre). Liste adresi otomatik oluşturulur; şifredeki özel karakterler kodlanır.
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

Gereken kütüphaneler: vita2d, libcurl (mbedtls ile), SceAvPlayer stub.

## PC'de test

    gcc -Wall -Isrc tests/test_host.c src/iptv.c -o test_host && ./test_host

M3U ayrıştırıcı, Xtream adresi ve sources.txt okuyucusu `src/iptv.c` içinde Vita'dan bağımsızdır ve bu testle doğrulanmıştır.

## Bilinen sınırlar

- `src/player.c` Vita üzerinde denenmedi. Görüntü bozuk ya da renkler yanlışsa `player.c` başındaki `FRAME_ALIGN`, `CHROMA_VU`, `DECIMATE` ayarlarına bak.
- Video şimdilik yazılımla RGB'ye çevrilip yarı çözünürlükte çiziliyor (`DECIMATE 2`). GPU ile renk dönüşümü sonraki adım olabilir.
- HLS (.m3u8) ve çeşitli codec'lerin SceAvPlayer'da çalışması yayına bağlıdır. Vita donanımı H.264 + AAC/MP3 çözer; HEVC çözmez.
- Liste indirme sırasında arayüz donar (tek iş parçacığı). Çok büyük listeler (24 MB üstü) reddedilir.
- Varsayılan yazı tipi Türkçe karakterlerin hepsini göstermeyebilir.
- HTTPS sertifikası doğrulanmıyor (Vita'da CA paketi yok).
