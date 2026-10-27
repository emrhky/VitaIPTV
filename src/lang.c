#include "lang.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int g_lang = LANG_EN;
void lang_set(int lang) { g_lang = lang == LANG_TR ? LANG_TR : LANG_EN; }
int  lang_get(void) { return g_lang; }

typedef struct { const char *en, *tr; } Pair;

static const Pair FIXED[] = {
    /* 0.8.1: PIN, guide, subtitles, theme */
    { "Type the new PIN again", "Yeni PIN'i tekrar yazın" },
    { "The two PINs are not the same: not changed", "İki PIN aynı değil: değiştirilmedi" },
    { "The new PIN is used when you save", "Yeni PIN kaydedince geçerli olur" },
    { "The lock is removed when you save", "Kilit kaydedince kaldırılır" },
    { "No programme guide", "Yayın akışı yok" },
    { "Size", "Boyut" },
    { "Colour", "Renk" },
    { "Place", "Yer" },
    { "Background", "Arka plan" },
    { "Small", "Küçük" },
    { "Medium", "Orta" },
    { "Large", "Büyük" },
    { "Very large", "Çok büyük" },
    { "White", "Beyaz" },
    { "Yellow", "Sarı" },
    { "Green", "Yeşil" },
    { "Blue", "Mavi" },
    { "Bottom", "Alt" },
    { "A little higher", "Biraz yukarı" },
    { "Top", "Üst" },
    { "Light band", "Hafif şerit" },
    { "Dark box", "Koyu kutu" },
    { "None", "Yok" },
    { "Theme", "Tema" },
    { "Purple", "Mor" },
    { "Orange", "Turuncu" },
    /* 0.8.0: guide, catch-up, details, series, PIN, sleep timer, number zap, search */
    { "Guide", "Rehber" }, { "Schedule", "Yayın Akışı" }, { "Details", "Detaylar" }, { "Live", "Canlı" },
    { "Today", "Bugün" }, { "Tomorrow", "Yarın" }, { "Yesterday", "Dün" }, { "NOW", "ŞİMDİ" }, { "ARCHIVE", "ARŞİV" },
    { "Loading...", "Yükleniyor..." }, { "Loading the guide...", "Rehber yükleniyor..." },
    { "No programme information", "Program bilgisi yok" }, { "No programme information at this time", "Bu saat için program bilgisi yok" },
    { "No programme information for this day", "Bu gün için program bilgisi yok" },
    { "No programme guide for this channel", "Bu kanalın yayın akışı yok" }, { "No programme guide for this list", "Bu listenin yayın akışı yok" },
    { "%s  \xC2\xB7  %s - %s  \xC2\xB7  %d min", "%s  \xC2\xB7  %s - %s  \xC2\xB7  %d dk" },
    { "%s %s - %s  \xC2\xB7  %d min", "%s %s - %s  \xC2\xB7  %d dk" }, { "%s  \xC2\xB7  playing", "%s  \xC2\xB7  oynatılıyor" },
    { "%d min left", "%d dk kaldı" }, { "%d h %d min", "%d sa %d dk" }, { "%d min", "%d dk" },
    { "%d min watched  \xC2\xB7  %d min left", "%d dk izlendi  \xC2\xB7  %d dk kaldı" }, { "Next: %s (%s)", "Sonraki: %s (%s)" },
    { "archive %d days", "arşiv %d gün" },
    { "Watch from the start (archive)", "Baştan izle (arşiv)" }, { "Watch live", "Canlı izle" }, { "Go to the live channel", "Canlı kanala geç" },
    { "In the archive: watch from the start", "Arşivde: baştan izlenebilir" },
    { "X: watch from the start (archive) or live", "X: baştan (arşiv) ya da canlı izle" },
    { "In the archive: X watches it from the start", "Arşivde: X ile baştan izlenir" },
    { "That programme is not in the archive", "Bu program arşivde yok" }, { "Not on yet", "Henüz yayınlanmadı" },
    { "No later programme in the guide", "Rehberde sonraki program yok" }, { "No earlier programme in the archive", "Arşivde önceki program yok" },
    { "Next programme in %d s", "Sonraki program %d sn içinde" },
    { "Watch", "İzle" }, { "Time", "Saat" }, { "Day", "Gün" }, { "Choose", "Seç" }, { "Programme", "Program" },
    { "Button", "Düğme" }, { "Previous / next", "Önceki / sonraki" }, { "Season", "Sezon" }, { "Episode", "Bölüm" },
    { "Continue (%s)", "Devam et (%s)" }, { "From the start", "Baştan" }, { "Favourite", "Favori" },
    { "Director:", "Yönetmen:" }, { "Cast:", "Oyuncular:" }, { "Season %d", "Sezon %d" }, { "Episodes", "Bölümler" },
    { "%d season", "%d sezon" }, { "%d seasons", "%d sezon" },
    { "Details and poster come from the provider; without them only the name shows.",
      "Detaylar ve kapak sağlayıcıdan gelir; yoksa yalnız ad görünür." },
    { "Reorder favourites", "Favorileri Sırala" },
    { "Up / down moves the channel, X saves the order", "Yukarı / aşağı kanalı taşır, X sırayı kaydeder" },
    { "Searching every film...", "Tüm filmlerde aranıyor..." }, { "Searching every series...", "Tüm dizilerde aranıyor..." },
    { "Search every film", "Tüm filmlerde ara" }, { "Search every series", "Tüm dizilerde ara" },
    { "Channel number", "Kanal numarası" }, { "Channel number...", "Kanal numarası..." },
    { "No channel with that number", "Bu numarada kanal yok" },
    { "Sleep timer:  < Off >", "Uyku zamanlayıcı:  < Kapalı >" },
    { "Sleep timer:  < %d min >   (%d min left)", "Uyku zamanlayıcı:  < %d dk >   (%d dk kaldı)" },
    { "Sleep timer: stops in %d s", "Uyku zamanlayıcı: %d sn sonra duracak" }, { "X: 15 more minutes", "X: 15 dakika daha" },
    { "Sleep timer: playback stopped", "Uyku zamanlayıcı: oynatma durduruldu" },
    { "Left / Right / X: change   O: close", "Sol / Sağ / X: değiştir   O: kapat" },
    { "Parental lock (PIN)", "Ebeveyn kilidi (PIN)" }, { "Wrong PIN", "Yanlış PIN" },
    { "New PIN (4-6 digits, empty: no lock)", "Yeni PIN (4-6 rakam, boş: kilit yok)" },
    { "The PIN needs 4 to 6 digits", "PIN 4 ile 6 rakam olmalı" },
    { "No subtitles in this film (MKV / MP4 text subtitles only)", "Bu filmde altyazı yok (yalnız MKV / MP4 metin altyazıları)" },
    /* 0.5.0: favourites */
    { "Favorites", "Favoriler" }, { "Favorite", "Favori" }, { "Added to favorites", "Favorilere eklendi" },
    { "Removed from favorites", "Favorilerden çıkarıldı" }, { "The favorites list is full", "Favori listesi dolu" },
    { "Could not add it to the favorites", "Favorilere eklenemedi" },   /* 0.6.0: films and series */
    { "Series added to favorites", "Dizi favorilere eklendi" }, { "Series removed from favorites", "Dizi favorilerden çıkarıldı" },
    /* 0.4.5 */
    { "Fav.", "Fav." }, { "Res.", "Çöz." }, { "P. Channel", "Ö. Kanal" },
    { "When needed", "Gerektiğinde" }, { "Always", "Her zaman" },
    { "Recently Watched", "Son İzlenenler" }, { "Reorder", "Taşı" }, { "Order saved", "Sıra kaydedildi" },
    { "Order not changed", "Sıra değiştirilmedi" },
    { "Up / down", "Yukarı / aşağı" },
    { "built", "derleme" },
    { "Now", "Şimdi" }, { "Next", "Sonra" },
    /* 0.7.0 */
    { "Picture", "Görüntü" }, { "Subtitles", "Altyazı" }, { "More", "Diğer" }, { "Forced", "Zorunlu" },
    { "Sync:  < %+d.%d s >", "Senkron:  < %+d.%d sn >" }, { "Sync:  < -0.%d s >", "Senkron:  < -0.%d sn >" },
    { "No subtitles", "Altyazı yok" }, { "Chosen by the server", "Sunucu seçiyor" }, { "No other sound", "Başka ses yok" },
    { "Left / Right: earlier / later   X: back to 0   O: close", "Sol / Sağ: erken / geç   X: sıfırla   O: kapat" },
    { "L / R: tab   X: choose   O: close", "L / R: sekme   X: seç   O: kapat" },
    { "Next episode in %d s", "Sonraki bölüm %d sn içinde" }, { "X: now   O: cancel", "X: hemen   O: iptal" },
    { "Clear watched episodes", "İzlenme Kayıtlarını Temizle" }, { "Clear watched episodes?", "İzlenme kayıtları temizlensin mi?" },
    { "The ticks of watched episodes are removed. Where films stopped is kept.",
      "İzlenen bölümlerin işaretleri silinir. Filmlerin kaldığı yerler korunur." },
    { "Watched episodes cleared", "İzlenme kayıtları temizlendi" },
    /* 0.5.0 */
    { "Through the Server", "Sunucu Üzerinden" }, { "Direct (without the server)", "Doğrudan (sunucusuz)" },
    { "Clear Recently Watched", "Son İzlenenleri Temizle" }, { "Clear recently watched?", "Son izlenenler temizlensin mi?" },
    { "The recently watched channels of every playlist are removed. Favorites stay.",
      "Bütün listelerin son izlenen kanalları silinir. Favoriler kalır." },
    { "Recently watched cleared", "Son izlenenler temizlendi" }, { "Nothing to clear", "Temizlenecek bir şey yok" },
    /* 0.4.0 */
    { "No channel before this one yet", "Henüz önceki kanal yok" },
    { "The channel before is not in this list", "Önceki kanal bu listede değil" },
    { "Sound only (radio)", "Sadece ses (radyo)" }, { "%dx%d (the only size)", "%dx%d (tek boyut)" },
    { "Size not known yet", "Boyut henüz belli değil" }, { "Stream", "Yayın" }, { "Delete playlist", "Listeyi sil" },
    { "Hide channels named 4K / UHD", "Adında 4K/UHD geçen kanalları gizle" }, { "Close", "Kapat" },
    { "IPTV player for the PS Vita: live TV, films, series, radio.", "PS Vita için IPTV oynatıcı: canlı TV, film, dizi, radyo." },
    { "M3U and Xtream playlists \xC2\xB7 HLS, MPEG-TS, MKV, MP4, FLV \xC2\xB7 HTTPS",
      "M3U ve Xtream listeleri \xC2\xB7 HLS, MPEG-TS, MKV, MP4, FLV \xC2\xB7 HTTPS" },
    { "H.264 hardware decoder up to 1080p.", "H.264 donanım çözücüyle 1080p'ye kadar." },
    { "MIT License \xC2\xB7 third-party notices in the VPK", "MIT Lisansı \xC2\xB7 üçüncü taraf bildirimleri VPK içinde" },
    { "No internet connection. Connect to Wi-Fi in Settings > Network.",
      "İnternet bağlantısı yok. Ayarlar > Ağ'dan Wi-Fi'ye bağlanın." },
    /* buttons */
    { "Open", "Aç" }, { "Add", "Ekle" }, { "Edit", "Düzenle" }, { "Delete", "Sil" }, { "Menu", "Menü" },
    { "Play", "Oynat" }, { "Search", "Ara" }, { "Group", "Grup" }, { "Page", "Sayfa" }, { "Back", "Geri" },
    { "New search", "Yeni arama" }, { "Clear", "Temizle" }, { "Channel", "Kanal" }, { "Info", "Bilgi" },
    { "Server", "Sunucu" }, { "Analyze", "Analiz" }, { "Station", "İstasyon" }, { "Again", "Tekrar" },
    { "Move", "Gezin" }, { "Edit / choose", "Düzenle / seç" }, { "Cancel", "İptal" },
    { "Yes", "Evet" }, { "No", "Hayır" }, { "Category", "Kategori" }, 
    { "Channels", "Kanallar" },
    /* playlists and channels */
    { "Playlists", "Listeler" }, { "%d playlists", "%d liste" },
    { "No playlists yet: press Square to add one", "Henüz liste yok: eklemek için Kare'ye basın" },
    { "Press Square to add your first playlist.", "İlk listenizi eklemek için Kare'ye basın." },
    { "All", "Tümü" }, { "%d channels", "%d kanal" }, { "%d found", "%d sonuç" }, { "Search: \"%s\"", "Arama: \"%s\"" },
    { "Nothing found", "Sonuç yok" }, { "No channels", "Kanal yok" }, { "Search channels", "Kanal ara" },
    { "Loading channels...", "Kanallar yükleniyor..." }, { "Loading channel list...", "Kanal listesi yükleniyor..." },
    { "Checking the account...", "Hesap kontrol ediliyor..." }, { "Loading categories...", "Kategoriler yükleniyor..." },
    { "Cancelling...", "İptal ediliyor..." }, { "Cancelled", "İptal edildi" },
    { "Wrong username or password", "Kullanıcı adı veya şifre yanlış" },
    { "Loading playlists...", "Listeler yükleniyor..." }, { "Starting...", "Başlatılıyor..." },
    { "Add playlist", "Liste ekle" }, { "Settings", "Ayarlar" }, { "Reload sources", "Listeleri yenile" },
    { "About", "Hakkında" }, { "Quit", "Çıkış" },
    { "Delete this playlist?", "Bu liste silinsin mi?" }, { "\"%s\" will be removed from the list.", "\"%s\" listeden kaldırılacak." },
    { "Deleted", "Silindi" }, { "Saved", "Kaydedildi" }, { "Settings saved", "Ayarlar kaydedildi" },
    { "Saved as Xtream (faster; live TV, films and series)", "Xtream olarak kaydedildi (daha hızlı; canlı TV, filmler ve diziler)" },
    /* films and series */
    { "Live TV", "Canlı TV" }, { "Films", "Filmler" }, { "Series", "Diziler" },
    { "Loading films...", "Filmler yükleniyor..." }, { "Loading series...", "Diziler yükleniyor..." },
    { "Loading episodes...", "Bölümler yükleniyor..." }, { "No films on this account", "Bu hesapta film yok" },
    { "No series on this account", "Bu hesapta dizi yok" }, { "No episodes found", "Bölüm bulunamadı" },
    { "This category is empty", "Bu kategori boş" }, { "%d films", "%d film" }, { "%d series", "%d dizi" },
    { "%d episodes", "%d bölüm" }, { "Press X to open this category", "Bu kategoriyi açmak için X'e basın" },
    { "Continue from %s", "%s noktasından devam et" }, { "Start from the beginning", "Baştan başla" },
    { "Pause", "Duraklat" }, { "10 s", "10 sn" }, { "5 min", "5 dk" }, { "The end", "Bitti" },
    { "This server does not allow jumping in the file", "Bu sunucu dosyada ileri/geri atlamaya izin vermiyor" },
    { "Jumping is not possible in this file", "Bu dosyada ileri/geri atlanamıyor" },
    { "AVI file: not supported (MKV, MP4, TS and FLV are)", "AVI dosyası: desteklenmiyor (MKV, MP4, TS ve FLV destekleniyor)" },
    { "Unknown file type", "Bilinmeyen dosya türü" },
    { "Damaged or unusual Matroska file", "Bozuk veya alışılmadık Matroska dosyası" },
    { "MP4 index too big", "MP4 dizini çok büyük" }, { "Damaged MP4 file (no index)", "Bozuk MP4 dosyası (dizin yok)" },
    { "MP4 file: the server does not allow reading its index", "MP4 dosyası: sunucu dizinin okunmasına izin vermiyor" },
    { "MP4 file: the server does not send parts of files (needed for MP4)", "MP4 dosyası: sunucu dosyanın parçalarını göndermiyor (MP4 için gerekli)" },
    { "The server stopped sending parts of the file", "Sunucu dosyanın parçalarını göndermeyi bıraktı" },
    { "The server did not answer", "Sunucu yanıt vermedi" },
    { "Original", "Orijinal" },
    { "This video uses an H.264 feature the Vita decoder refuses (0x8062000F)", "Bu video, Vita çözücüsünün kabul etmediği bir H.264 özelliği kullanıyor (0x8062000F)" },
    { "Seek", "Sar" }, { "Live TV: jumping is not possible", "Canlı yayında ileri/geri sarılamaz" },
    { "This channel has one picture size. Another needs the transcoding server (channel list: Select > Settings)",
      "Bu kanalın tek bir görüntü boyutu var. Başkası için dönüştürme sunucusu gerekir (kanal listesi: Select > Ayarlar)" },
    { "Reading the film's header...", "Filmin başlığı okunuyor..." }, { "Finding the place in the film...", "Filmde yer aranıyor..." },
    { "Reading the film's index...", "Filmin dizini okunuyor..." }, { "Finding the length of the film...", "Filmin süresi bulunuyor..." },
    { "The connection is too slow for %dp: switched to a smaller picture", "Bağlantı %dp için yavaş: daha küçük görüntüye geçildi" },
    { "This film has one picture size (%dx%d). 720p needs the transcoding server (channel list: Select > Settings)",
      "Bu filmin tek bir görüntü boyutu var (%dx%d). 720p için dönüştürme sunucusu gerekir (kanal listesi: Select > Ayarlar)" },
    { "A smaller picture needs the transcoding server (channel list: Select > Settings)",
      "Daha küçük görüntü için dönüştürme sunucusu gerekir (kanal listesi: Select > Ayarlar)" },
    { "Edit playlist", "Listeyi düzenle" }, { "Type", "Tür" }, { "Name", "Ad" }, { "Server address", "Sunucu adresi" },
    { "Playlist link", "Liste bağlantısı" }, { "Stream link", "Yayın bağlantısı" }, { "Username", "Kullanıcı adı" },
    { "Username (if needed)", "Kullanıcı adı (gerekirse)" }, { "Password", "Şifre" }, { "Password (if needed)", "Şifre (gerekirse)" },
    { "Save", "Kaydet" }, { "(empty)", "(boş)" },
    { "Xtream (server + username + password)", "Xtream (sunucu + kullanıcı adı + şifre)" },
    { "M3U playlist (link)", "M3U listesi (bağlantı)" }, { "Single stream (link)", "Tek yayın (bağlantı)" },
    { "Tip: a pasted get.php link fills everything", "İpucu: yapıştırılan get.php bağlantısı her şeyi doldurur" },
    { "Server address (e.g. http://host:8080)", "Sunucu adresi (ör. http://host:8080)" }, { "Link", "Bağlantı" },
    { "Enter the link", "Bağlantıyı girin" }, { "Enter the server address", "Sunucu adresini girin" },
    { "Xtream needs a username and a password", "Xtream için kullanıcı adı ve şifre gerekli" },
    { "Too many sources", "Çok fazla liste" }, { "Could not write %s", "%s yazılamadı" },
    { "This entry is a file in ux0:data/VitaIPTV (delete or rename it there)",
      "Bu kayıt ux0:data/VitaIPTV içindeki bir dosya (orada silin veya yeniden adlandırın)" },
    { "The keyboard could not be opened (see log.txt)", "Klavye açılamadı (log.txt)" },
    /* settings */
    { "Transcoding server", "Dönüştürme sunucusu" }, { "Use automatically", "Otomatik kullan" },
    { "On", "Açık" }, { "Off", "Kapalı" }, { "Test server", "Sunucuyu dene" },
    { "Transcoding server (e.g. http://192.168.1.20:8090)", "Dönüştürme sunucusu (ör. http://192.168.1.20:8090)" },
    { "Server: plays HEVC, 1080i, AC-3 and other formats (tools/vita_iptv_proxy.py)",
      "Sunucu: HEVC, 1080i, AC-3 ve diğer biçimleri oynatır (tools/vita_iptv_proxy.py)" },
    { "Enter the server address first", "Önce sunucu adresini girin" }, { "Bad server address", "Geçersiz sunucu adresi" },
    { "Testing the server...", "Sunucu deneniyor..." }, { "Server is running", "Sunucu çalışıyor" },
    { "No answer from the server: %s", "Sunucudan yanıt yok: %s" }, { "unexpected reply", "beklenmeyen yanıt" },
    { "Language", "Dil" }, { "Automatic (system)", "Otomatik (sistem)" }, { "Hide adult channels", "Yetişkin kanallarını gizle" },
    { "loaded", "yüklendi" }, 
    /* player */
    { "Connecting...", "Bağlanıyor..." }, { "Waiting for the stream", "Yayın bekleniyor" },
    { "Starting video...", "Görüntü başlatılıyor..." }, { "Waiting for a keyframe", "Anahtar kare bekleniyor" },
    { "Stream ended", "Yayın bitti" }, { "Cannot play this channel", "Bu kanal oynatılamıyor" }, { "Cannot start", "Başlatılamadı" },
    { "Cannot play this file", "Bu dosya oynatılamıyor" }, { "No video after 20 s", "20 sn içinde görüntü gelmedi" },
    { "See ux0:data/VitaIPTV/log.txt", "Ayrıntı: ux0:data/VitaIPTV/log.txt" },
    { "Preferred resolution", "Tercih edilen çözünürlük" }, { "Automatic", "Otomatik" }, { "Lowest", "Düşük" },
    { "Resolution", "Çözünürlük" }, { "No picture in %s: switched to %s", "%s kalitesinde görüntü yok, %s seçildi" }, 
    { "Tip: a transcoding server on your PC can play this channel (channel list: Select > Settings, see README)",
      "İpucu: bilgisayarınızdaki dönüştürme sunucusu bu kanalı oynatabilir (kanal listesi: Select > Ayarlar, README)" },
    
    { "Played through the transcoding server. Is it running? (channel list: Select > Settings > Test)",
      "Dönüştürme sunucusu üzerinden oynatıldı. Sunucu çalışıyor mu? (kanal listesi: Select > Ayarlar > Sunucuyu dene)" },
    { "No sound: press Square to play through the server", "Ses yok: sunucu üzerinden oynatmak için Kare'ye basın" },
    { "via server", "sunucu üzerinden" }, { "  via server", "  sunucu üzerinden" },
    { "Video %dx%d%s   shown %u   dropped %u   late %u   damaged %u   errors %u   %u KB",
      "Görüntü %dx%d%s   gösterilen %u   atlanan %u   geç %u   hasarlı %u   hata %u   %u KB" },
    
    { "%d%%   (network %u KB/s, the film needs %u KB/s)", "%d%%   (bağlantı %u KB/s, filmin ihtiyacı %u KB/s)" },
    { "Audio %s %s %s   A/V %+d ms", "Ses %s %s %s   A/V %+d ms" }, { "Audio %s %s %s", "Ses %s %s %s" },
    /* the info table (Triangle) */
    { "Video", "Görüntü" }, { "Frame rate", "Kare hızı" }, { "Shown", "Gösterilen" }, { "Dropped", "Atlanan" },
    { "Audio", "Ses" }, { "Waits", "Beklemeler" }, { "Damaged / errors", "Hasarlı / hata" }, { "Network", "Bağlantı" },
    { "Needed", "İhtiyaç" }, { "Received", "Alınan" }, { "Not decoded", "Çözülmeyen" }, { "off", "kapalı" },
    { "waiting", "bekleniyor" },
    { "1080p at %d fps: the Vita decodes 1080p up to about 30 fps, some pictures are left out",
      "1080p %d fps: Vita 1080p'yi en fazla yaklaşık 30 fps çözebilir, bazı kareler atlanıyor" },
    { "The Vita cannot decode this 1080p at %d fps: switched to 720p", "Vita bu 1080p yayını %d fps hızında çözemiyor: 720p'ye geçildi" },
    { "Referer (if needed)", "Referer (gerekirse)" }, { "User-Agent (if needed)", "User-Agent (gerekirse)" },
    { "Referer (e.g. https://site/page)", "Referer (örn. https://site/sayfa)" }, { "User-Agent", "User-Agent" },
    { "Waiting for the network...", "Ağ bekleniyor..." },
    { "Radio", "Radyo" }, { "ON AIR", "YAYINDA" }, { "Buffering...", "Arabelleğe alınıyor..." },
    { "Matroska (MKV)", "Matroska (MKV)" },
    { "Stream analysis", "Yayın analizi" }, { "Analyzing stream...", "Yayın analiz ediliyor..." },
    { "Cannot start analysis", "Analiz başlatılamadı" }, { "Cannot start the TS player", "TS oynatıcı başlatılamadı" },
    { "Live TV and radio on your Vita", "Vita'nızda canlı TV ve radyo" },
    /* messages without numbers */
    { "HEVC (H.265) video: the Vita cannot decode it", "HEVC (H.265) görüntü: Vita bunu çözemiyor" },
    
    { "The server sent a web page instead of video", "Sunucu görüntü yerine bir web sayfası gönderdi" },
    { "Server sent a web page, not video", "Sunucu görüntü yerine bir web sayfası gönderdi" },
    { "No MPEG-TS data (not a TS stream?)", "MPEG-TS verisi yok (TS yayını değil mi?)" },
    { "No video stream (radio channel?)", "Görüntü akışı yok (radyo kanalı mı?)" },
    { "Channel is encrypted (scrambled)", "Kanal şifreli" }, { "No video data received", "Görüntü verisi gelmedi" },
    { "Cannot create threads", "İş parçacıkları oluşturulamadı" }, { "Cannot open file", "Dosya açılamıyor" },
    { "Out of memory", "Bellek yetersiz" }, { "Bad address", "Geçersiz adres" }, { "Request failed", "İstek başarısız" },
    { "HTTP init failed", "HTTP başlatılamadı" }, { "No data received", "Veri gelmedi" }, { "Read error", "Okuma hatası" },
    { "Empty answer", "Boş yanıt" }, { "List is too large (over 24 MB)", "Liste çok büyük (24 MB üstü)" },
    { "URL too long", "Adres çok uzun" },
    { "No live channels returned (check host, username, password)", "Canlı kanal gelmedi (sunucu, kullanıcı adı ve şifreyi kontrol edin)" },
};

/* messages with numbers: %d %u %s %08X in the English text, %s placeholders (same order) in the Turkish one */
static const Pair PATTERNS[] = {
    /* (0.5.0: was among the fixed texts, so the message with its number in it stayed English) */
    { "The Vita decoder gives no picture for this stream (%u errors). The transcoding server can play it",
      "Vita çözücüsü bu yayından görüntü vermiyor (%s hata). Dönüştürme sunucusuyla izlenebilir" },
    { "%dx%d is above the Vita decoder limit (720p)", "%sx%s, Vita çözücüsünün sınırının (720p) üstünde" },
    { "%dx%d is above the Vita decoder limit (1080p)", "%sx%s, Vita çözücüsünün sınırının (1080p) üstünde" },
    { "%dx%d is above 720p", "%sx%s, 720p üstünde" },
    { "1080p decoder could not start (0x%08X), see log.txt", "1080p çözücü başlatılamadı (0x%s), log.txt'ye bakın" },
    { "Account in use: %d of %d connections (channels may not open)", "Hesap kullanımda: %s / %s bağlantı (kanallar açılmayabilir)" },
    { "Account %s", "Hesap durumu: %s" },
    { "Server answered HTTP %d", "Sunucu HTTP %s yanıtı verdi" },
    { "Server answered HTTP %d (the provider refuses: account busy?)", "Sunucu HTTP %s yanıtı verdi (sağlayıcı reddediyor: hesap kullanımda olabilir)" },
    { "Connection lost (%d times)", "Bağlantı koptu (%s kez)" },
    { "Connection failed (0x%08X)", "Bağlantı kurulamadı (0x%s)" },
    { "The server did not answer (0x%08X)", "Sunucu yanıt vermedi (0x%s)" },
    { "Connection failed (%s)", "Bağlantı kurulamadı (%s)" },
    { "HTTPS failed (0x%08X): this site needs newer TLS than the Vita has. Copy the .m3u file to ux0:data/VitaIPTV/ instead",
      "HTTPS başarısız (0x%s): bu site Vita'dakinden yeni TLS istiyor. .m3u dosyasını PC'den ux0:data/VitaIPTV/ klasörüne kopyalayın" },
    { "Bad address (0x%08X)", "Geçersiz adres (0x%s)" }, { "Request failed (0x%08X)", "İstek başarısız (0x%s)" },
    { "Read error (0x%08X)", "Okuma hatası (0x%s)" }, { "HTTP init failed (0x%08X)", "HTTP başlatılamadı (0x%s)" },
    { "Stream stopped (connection lost %d times)", "Yayın durdu (bağlantı %s kez koptu)" },
    { "No usable keyframe in %u pictures", "%s karede kullanılabilir anahtar kare yok" },
    { "Decoder produced no picture (%u errors). Switch the Vita fully off and on again, then try a 720p channel",
      "Çözücü görüntü üretmedi (%s hata). Vita'yı tamamen kapatıp açın, sonra 720p kanalı deneyin" },
    { "Decoder init failed (0x%08X), see log.txt", "Çözücü başlatılamadı (0x%s), log.txt'ye bakın" },
    { "Interlaced video (%dx%d, TV broadcast): the Vita decoder refuses it",
      "Geçmeli (interlaced) görüntü (%sx%s, TV yayını): Vita çözücüsü kabul etmiyor" },
    { "Server answered HTTP 407 (the provider refuses: account busy or channel not in the package)",
      "Sunucu HTTP 407 dedi (sağlayıcı reddediyor: hesap başka yerde açık ya da kanal pakette yok)" },
    { "HTTPS failed (0x%08X): this site needs newer TLS than the Vita has",
      "HTTPS başarısız (0x%s): bu site Vita'dakinden yeni TLS istiyor" },
    { "No HLS variant answers", "HLS yayınının hiçbir kalitesi yanıt vermiyor" },
    { "Stream stopped (the HLS playlist is not updated)", "Yayın durdu (HLS listesi güncellenmiyor)" },
    { "Encrypted HLS stream (AES): not supported", "Şifreli HLS yayını (AES): desteklenmiyor" },
    { "HLS with MP4 segments: not supported (MPEG-TS only)", "MP4 parçalı HLS: desteklenmiyor (yalnız MPEG-TS)" },
    { "HLS playlist without a usable stream", "HLS listesinde oynatılabilir yayın yok" },
    { "HLS playlist without segments", "HLS listesinde parça yok" },
    { "HLS playlist inside a playlist: not supported", "İç içe HLS listesi: desteklenmiyor" },
    { "Decoder rejects this stream (0x%08X)", "Çözücü bu yayını kabul etmiyor (0x%s)" },
    { "No memory layout accepted by the decoder (0x%08X)", "Çözücü hiçbir bellek düzenini kabul etmedi (0x%s)" },
    { "Video is %s: not supported", "Görüntü %s: desteklenmiyor" },
    { "H.264 %d-bit / chroma %d: not supported", "H.264 %s-bit / renk %s: desteklenmiyor" },
    { "Picture size changed to %dx%d (not supported yet)", "Görüntü boyutu %sx%s oldu (henüz desteklenmiyor)" },
    { "Audio %s: not supported", "Ses %s: desteklenmiyor" },
    { "Audio has %d channels: not supported", "Ses %s kanallı: desteklenmiyor" },
    { "Audio rate %d Hz: not supported", "Ses %s Hz: desteklenmiyor" },
    { "Audio decoder failed (0x%08X)", "Ses çözücü başlatılamadı (0x%s)" },
    { "Audio decode failed (0x%08X)", "Ses çözülemedi (0x%s)" },
    { "Audio output failed (0x%08X)", "Ses çıkışı açılamadı (0x%s)" },
    { "AAC Main audio: the Vita cannot play it (0x%08X)", "AAC Main ses: Vita bunu çalamıyor (0x%s)" },
    { "AAC SSR audio: not supported", "AAC SSR ses: desteklenmiyor" },
    { "The film changed on the server: open it again", "Film sunucuda değişmiş: yeniden açın" },
    { "Player error 0x%08X (see ux0:data/VitaIPTV/log.txt)", "Oynatıcı hatası 0x%s (ux0:data/VitaIPTV/log.txt)" },
    { "No channels found in %s", "%s içinde kanal bulunamadı" },
    { "failed (0x%08X)", "yüklenemedi (0x%s)" },
    { "Failed: %s", "Başarısız: %s" },
};

const char *T(const char *en)
{
    if (g_lang != LANG_TR || !en) return en;
    for (size_t i = 0; i < sizeof FIXED / sizeof FIXED[0]; i++)
        if (!strcmp(en, FIXED[i].en)) return FIXED[i].tr;
    return en;
}

/* Matches msg against an English printf-style pattern; captured values go to caps. */
static int match(const char *pat, const char *s, char caps[4][96], int *nc)
{
    *nc = 0;
    while (*pat) {
        if (*pat == '%') {
            pat++;
            while (isdigit((unsigned char)*pat)) pat++;
            char conv = *pat ? *pat++ : 0;
            const char *start = s;
            if (conv == 'd' || conv == 'u') { if (*s == '-') s++; while (isdigit((unsigned char)*s)) s++; }
            else if (conv == 'X' || conv == 'x') { while (isxdigit((unsigned char)*s)) s++; }
            else { char stop = *pat; while (*s && *s != stop) s++; }
            if (s == start || *nc >= 4) return 0;
            size_t n = (size_t)(s - start);
            if (n >= sizeof caps[0]) n = sizeof caps[0] - 1;
            memcpy(caps[*nc], start, n);
            caps[(*nc)++][n] = 0;
        } else {
            if (*pat != *s) return 0;
            pat++;
            s++;
        }
    }
    return *s == 0;
}

const char *T_msg(const char *en)
{
    static char bufs[4][320];
    static int k;
    if (g_lang != LANG_TR || !en) return en;
    const char *f = T(en);
    if (f != en) return f;
    for (size_t i = 0; i < sizeof PATTERNS / sizeof PATTERNS[0]; i++) {
        char caps[4][96];
        int nc;
        if (!match(PATTERNS[i].en, en, caps, &nc)) continue;
        char *out = bufs[k++ & 3];
        size_t o = 0;
        int ci = 0;
        for (const char *t = PATTERNS[i].tr; *t && o + 1 < sizeof bufs[0]; t++) {
            if (t[0] == '%' && t[1] == 's') {
                const char *c = ci < nc ? caps[ci++] : "";
                const char *inner = T(c);                    /* captured words may have a translation too */
                while (*inner && o + 1 < sizeof bufs[0]) out[o++] = *inner++;
                t++;
            } else out[o++] = *t;
        }
        out[o] = 0;
        return out;
    }
    return en;
}
