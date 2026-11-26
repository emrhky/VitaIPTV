#include "lang.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int g_lang = LANG_EN;
void lang_set(int lang) { g_lang = lang == LANG_TR ? LANG_TR : LANG_EN; }
int  lang_get(void) { return g_lang; }

typedef struct { const char *en, *tr; } Pair;

static const Pair FIXED[] = {
    /* 1.1: sound (Select > Audio) */
    { "Volume boost", "Ses yükseltme" },
    { "Clear sound", "Ses netleştirme" },
    { "Clear speech (5.1)", "Konuşma netleştirme (5.1)" },
    { "Same volume on every channel", "Kanallar arası ses eşitleme" },
    { "Sound field", "Ses alanı" }, { "Speakers", "Hoparlör" }, { "Headphones", "Kulaklık" }, { "Auto", "Otomatik" },   /* 1.3 */
    { "Sound delay", "Ses gecikmesi" },   /* 1.2.1 */
    { "Sound levelling", "Ses dengeleme" }, { "Between channels", "Kanallar arası" }, { "Night", "Gece" }, { "s", "sn" },   /* 1.2.2 */
    /* 1.5.1 */
    { "The server sent nothing (an empty answer)", "Sunucu bir şey göndermedi (boş cevap)" },
    /* 1.2.4d: films given as HLS */
    { "The film's playlist did not come whole", "Filmin oynatma listesi tam gelmedi" },
    { "The server sent something that is not a film", "Sunucu film olmayan bir şey gönderdi" },
    { "This film's sound comes apart from the picture (HLS): it cannot be downloaded", "Bu filmin sesi görüntüden ayrı geliyor (HLS): indirilemez" },
    { "This film has no picture the Vita can play", "Bu filmde Vita'nın oynatabileceği bir görüntü yok" },
    { "The film's playlist cannot be read", "Filmin oynatma listesi okunamıyor" },
    { "This film comes in a form that cannot be downloaded (HLS with MP4 parts)", "Bu film indirilemeyen bir biçimde geliyor (MP4 parçalı HLS)" },
    { "This film is protected (DRM): it cannot be downloaded", "Bu film korumalı (DRM): indirilemez" },
    { "This is a live stream: it cannot be downloaded", "Bu canlı bir yayın: indirilemez" },
    { "This film is encrypted and this build cannot decrypt it", "Bu film şifreli ve bu sürüm şifresini çözemiyor" },
    { "A part of the film is too big", "Filmin bir parçası çok büyük" },
    { "The film's key cannot be fetched", "Filmin anahtarı alınamıyor" },
    { "The film's key is wrong", "Filmin anahtarı yanlış" },
    { "This film's parts are not MPEG-TS: it cannot be downloaded", "Bu filmin parçaları MPEG-TS değil: indirilemez" },
    { "The Vita slept: the stream was stopped", "Vita uykuya geçti: yayın durduruldu" },
    { "Next %d...  (%d of %d shown)", "Sonraki %d...  (%d / %d gösteriliyor)" },
    { "Most watched", "En çok izlenen" }, { "Newest added", "En yeni eklenen" }, { "Year (newest first)", "Yıl (yeniden eskiye)" },
    { "Title (A-Z)", "Ad (A-Z)" }, { "Order", "Sıralama" }, { "Search the whole archive", "Tüm arşivde ara" },
    { "Show the whole list", "Tüm listeyi göster" }, { "Search the archive", "Arşivde ara" }, { "Loading the archive...", "Arşiv yükleniyor..." },
    { "The video is WebM / Ogg (VP8, VP9, Theora pictures): the Vita cannot play it", "Video WebM / Ogg (VP8, VP9, Theora görüntü): Vita oynatamaz" },
    /* 1.5: Internet Archive, web pages, black screen, the last searches */
    { "Internet Archive", "Internet Archive" }, { "Web page", "Web sayfası" },
    { "Internet Archive: free films of archive.org (an address or words)", "Internet Archive: archive.org'daki ücretsiz filmler (adres ya da kelimeler)" },
    { "Web page: the videos written in a page (no JavaScript)", "Web sayfası: bir sayfada yazılı videolar (JavaScript yok)" },
    { "Address or words", "Adres ya da kelimeler" }, { "Page link", "Sayfa bağlantısı" },
    { "archive.org address or words", "archive.org adresi ya da kelimeler" },
    { "Username (not needed)", "Kullanıcı adı (gerekmez)" }, { "Password (not needed)", "Şifre (gerekmez)" },
    { "archive.org/details/... (a collection or a film), archive.org/search?query=... or words (e.g. charlie chaplin).",
      "archive.org/details/... (bir koleksiyon ya da film), archive.org/search?query=... ya da kelimeler (ör. charlie chaplin)." },
    { "A page with a video in it. A site that makes its video with JavaScript does not work (webcheck.txt says why).",
      "İçinde video olan bir sayfa. Videosunu JavaScript ile açan siteler çalışmaz (nedenini webcheck.txt yazar)." },
    { "Enter an archive.org address or words to search for", "Bir archive.org adresi ya da aranacak kelimeler yazın" },
    { "Not an archive.org address (or words to search for)", "Bu bir archive.org adresi değil (ya da aranacak kelimeler)" },
    { "Searching the archive...", "Arşivde aranıyor..." }, { "Search (in descriptions)", "Ara (açıklamalarda)" }, { "Reading the page...", "Sayfa okunuyor..." },
    { "No films found there", "Orada film bulunamadı" }, { "No video found in this page", "Bu sayfada video bulunamadı" },
    { "The page is not there (HTTP 404)", "Sayfa yok (HTTP 404)" },
    { "The site refuses the app (HTTP 403: closed to this region or to apps)", "Site uygulamayı reddediyor (HTTP 403: bu bölgeye ya da uygulamalara kapalı)" },
    { "The site makes its video with JavaScript: it is not in the page", "Site videosunu JavaScript ile açıyor: sayfada yazılı değil" },
    { "The video is DASH (.mpd): the Vita player does not play it", "Video DASH (.mpd): Vita oynatıcısı oynatmıyor" },
    { "The video is H.265 (HEVC): the Vita cannot play it", "Video H.265 (HEVC): Vita oynatamaz" },
    { "The video is HLS with fMP4 parts: the Vita player does not play it", "Video fMP4 parçalı HLS: Vita oynatıcısı oynatmıyor" },
    { "The video is encrypted (AES HLS): the Vita player does not play it", "Video şifreli (AES HLS): Vita oynatıcısı oynatmıyor" },
    { "The video is encrypted (DRM): the Vita cannot play it", "Video şifreli (DRM): Vita oynatamaz" },
    { "This item has no MP4 (H.264) video up to 1080p: the Vita cannot play it", "Bu kayıtta 1080p'ye kadar MP4 (H.264) video yok: Vita oynatamaz" },
    { "This item has no video file", "Bu kayıtta video dosyası yok" }, { "The file's address is too long", "Dosyanın adresi çok uzun" },
    { "Black screen while it downloads", "İndirirken siyah ekran" },
    { "Waiting for the download  -  any button: back", "İndirme bekliyor  -  geri dönmek için bir tuşa basın" },
    { "Downloads ended  -  any button: back", "İndirmeler bitti  -  geri dönmek için bir tuşa basın" },
    { "New search...", "Yeni arama..." }, { "Opening...", "Açılıyor..." },
    { "Downloading %d%%  (%s/s)  -  any button: back", "İndiriliyor %%%d  (%s/sn)  -  geri dönmek için bir tuşa basın" },
    /* 1.4.1: downloads and playing the downloaded copy */
    { "Watching from the same server: the download waits", "Aynı sunucudan izleniyor: indirme bekliyor" },
    { "Playing the downloaded copy", "İndirilenden oynatılıyor" },
    { "An earlier video did not close and still holds memory: restart the app", "Önceki video kapanamadı ve belleği tutuyor: uygulamayı yeniden başlatın" },
    { "Playing the downloaded copy (still downloading)", "İndirilenden oynatılıyor (indirme sürüyor)" },
    { "Downloaded: %s", "İndirildi: %s" }, { "Could not download: %s", "İndirilemedi: %s" },
    { "Not enough free space on the memory card (%s free)", "Hafıza kartında yer yetmiyor (boş: %s)" },
    { "Try again", "Tekrar dene" }, { "Trying again", "Tekrar deneniyor" }, { "Could not download", "İndirilemedi" },
    /* 1.4: colours and backgrounds */
    { "Colour", "Renk" }, { "Background", "Arka plan" }, { "Teal", "Turkuaz" }, { "Pink", "Pembe" },
    { "Navy", "Lacivert" }, { "Black (OLED)", "Siyah (OLED)" }, { "Graphite", "Grafit" }, { "Warm dark", "Sıcak koyu" },
    { "The colour of the selection and the lines (seen at once).", "Seçimin ve çizgilerin rengi (hemen görünür)." },
    { "The background: navy, black (OLED: everything black, best on the first Vita's screen), graphite or warm dark.",
      "Arka plan: lacivert, siyah (OLED: her yer simsiyah, ilk Vita'nın ekranında en iyisi), grafit ya da sıcak koyu." },
    /* 1.3.2: watching while it downloads; the Downloads keys; Videos on the Vita */
    { "%d%% downloaded", "%%%d indi" }, { "Waiting for the download...", "İndirme bekleniyor..." },
    { "The rest has not been downloaded yet", "Gerisi henüz inmedi" },
    { "The download has not reached this part yet", "İndirme henüz bu kısma gelmedi" },
    { "This MP4 file can be played only when its download has ended", "Bu MP4 dosyası ancak indirmesi bitince oynatılabilir" },
    { "Stop all downloads", "Tüm indirmeleri durdur" }, { "Continue all downloads", "Tüm indirmeleri sürdür" },
    { "Downloads stopped", "İndirmeler durduruldu" }, { "Downloads go on", "İndirmeler sürüyor" }, { "Nothing to continue", "Sürdürülecek indirme yok" },
    { "This one can be watched when its download has ended", "Bu, indirmesi bitince izlenebilir" },
    { "A little more of it must be downloaded first", "Önce biraz daha inmesi gerekiyor" },
    { "Stopped: Square goes on where it stopped (the app must stay open)", "Durduruldu: Kare ile kaldığı yerden devam eder (uygulama açık kalmalı)" },
    { "Watch", "İzle" }, { "Stop", "Durdur" },
    { "Loading a list: the download waits", "Liste yükleniyor: indirme bekliyor" },
    { "The Vita does not let the app open this folder", "Vita bu klasörü uygulamaya açmıyor" },
    { "This folder cannot be opened (0x%08X)", "Bu klasör açılamıyor (0x%08X)" },
    { "Delete this video file?", "Bu video dosyası silinsin mi?" },
    { "Could not delete it (the Vita does not allow it here?)", "Silinemedi (Vita burada izin vermiyor olabilir)" },
    /* 1.3.1: Videos on the Vita; downloads */
    { "Videos on the Vita", "Vita'daki Videolar" },
    { "Video files on the memory card, folder by folder", "Hafıza kartındaki video dosyaları, klasör klasör" },
    { "VitaIPTV folder", "VitaIPTV klasörü" }, { "Video app", "Video uygulaması" },
    { "Memory card (ux0:)", "Hafıza kartı (ux0:)" }, { "USB / SD card (uma0:)", "USB / SD kart (uma0:)" },
    { "Internal storage (imc0:)", "Dahili bellek (imc0:)" }, { "Up", "Üst klasör" },
    { "No folders or video files here", "Burada klasör ya da video dosyası yok" },
    { "Choose a place; MKV, MP4, TS and FLV files play (an .srt beside one: its subtitles)",
      "Bir yer seçin; MKV, MP4, TS ve FLV dosyaları oynar (yanındaki .srt altyazı olur)" },
    { "Videos on the Vita: open it (X) and choose a folder", "Vita'daki Videolar: X ile açıp bir klasör seçin" },
    { "Download stopped. The provider may count it as watching for a few minutes",
      "İndirme durduruldu. Sağlayıcı bunu birkaç dakika daha izleme sayabilir" },
    { "Deleted. The provider may count the stopped download as watching for a few minutes",
      "Silindi. Sağlayıcı durdurulan indirmeyi birkaç dakika daha izleme sayabilir" },
    /* 1.3: sound and subtitles per stream; films on the memory card; downloads */
    { "Downloads", "İndirilenler" }, { "Download", "İndir" }, { "Download this season", "Bu sezonu indir" },
    { "Download every episode", "Tüm bölümleri indir" },
    { "Downloading: %s  %d%%  (%s/s)", "İndiriliyor: %s  %%%d  (%s/sn)" },
    { "Downloading: %s  %d%%  (%s/s)%s", "İndiriliyor: %s  %%%d  (%s/sn)%s" },
    { "Download waits for the Wi-Fi: %s%s", "İndirme Wi-Fi bekliyor: %s%s" },
    { "Download waits for a free connection of the account: %s%s", "İndirme hesapta boş bağlantı bekliyor: %s%s" },   /* 1.2.2 */
    { "  \xC2\xB7  %d waiting", "  \xC2\xB7  sırada %d" },
    { "Downloading: %s of %s  (%s/s)", "İndiriliyor: %s / %s  (%s/sn)" },
    { "Nothing yet: Select > Download on a film or an episode", "Henüz yok: bir film ya da bölümde Select > İndir" },
    { "%d films, %d series", "%d film, %d dizi" },
    { "%s  \xC2\xB7  %s  \xC2\xB7  free: %s", "%s  \xC2\xB7  %s  \xC2\xB7  boş: %s" },
    { "free: %s", "boş: %s" }, { "Waiting", "Sırada" }, { "Failed", "Hata" }, { "Stopped %d%%", "Durdu %%%d" },
    { "Delete series", "Diziyi sil" }, { "Continue", "Devam" },
    { "No downloaded films: Select > Download on a film", "İndirilmiş film yok: bir filmde Select > İndir" },
    { "No episodes", "Bölüm yok" },
    { "Posters", "Afişler" },   /* 1.2.4q */
    { "The server has no episodes of this series (it may have been removed)", "Sunucuda bu dizinin bölümü yok (kaldırılmış olabilir)" },
    { "Film and series posters seen before (at most 40 MB, the oldest go first). Cleared: they load from the server again.",
      "Daha önce görülen film ve dizi afişleri (en fazla 40 MB, önce en eskiler gider). Silinirse sunucudan yeniden yüklenir." },
    { "Not enough free space: %d MB needed, %d MB free", "Yetersiz alan: %d MB gerekli, %d MB boş" },
    { "Not enough free space on the memory card", "Hafıza kartında yeterli boş alan yok" },
    { "Cannot write to the memory card", "Hafıza kartına yazılamıyor" },
    { "The memory card is full", "Hafıza kartı doldu" },
    { "The download's information cannot be read", "İndirmenin bilgi dosyası okunamadı" },
    { "Stopped: X goes on where it stopped (the app must stay open)", "Durduruldu: X ile kaldığı yerden devam eder (uygulama açık kalmalı)" },
    { "Season %d, episode %d", "%d. sezon, %d. bölüm" },
    { "Delete this download?", "Bu indirme silinsin mi?" },
    { "Delete every downloaded episode of this series?", "Bu dizinin indirilen bütün bölümleri silinsin mi?" },
    /* 1.2.4: the order of the downloads, the watched ones */
    { "Waiting (%d)", "Sırada (%d)" }, { "Watched, %s", "İzlendi, %s" },
    { "Download it next", "Sıradaki yap" }, { "Move it up one", "Bir öne al" }, { "Download it last", "En sona al" },
    { "It downloads next", "Sıradaki indirme bu" }, { "It downloads last", "En sona alındı" }, { "Moved up", "Bir öne alındı" },
    { "It is not waiting any more", "Artık sırada değil" },
    { "Delete the watched episodes of this series", "Bu dizinin izlenen bölümlerini sil" },
    { "Delete all watched downloads", "İzlenen bütün indirmeleri sil" },
    { "Delete the %d watched episodes of this series?", "Bu dizinin izlenen %d bölümü silinsin mi?" },
    { "Delete the %d watched downloads?", "İzlenen %d indirme silinsin mi?" },
    { "Films and episodes watched to their end", "Sonuna kadar izlenen filmler ve bölümler" },
    { "Could not delete all of it", "Tamamı silinemedi" },
    { "Download stopped", "İndirme durduruldu" }, { "Download goes on", "İndirme devam ediyor" },
    { "Cannot write to %s", "%s klasörüne yazılamıyor" },
    { "Downloading: see Downloads on the home screen", "İndiriliyor: ana sayfadaki İndirilenler'e bakın" },
    { "%d downloads queued: see Downloads on the home screen", "%d indirme sıraya alındı: ana sayfadaki İndirilenler'e bakın" },
    { "Already in Downloads", "Zaten İndirilenler'de" },
    { "Little free space on the memory card", "Hafıza kartında boş alan az" },
    { "Downloads: open it (X); Triangle in it deletes", "İndirilenler: X ile açın; içinde Üçgen siler" },
    { "A download is running: an account with one connection cannot play at the same time (stop it in Downloads)",
      "İndirme sürüyor: tek bağlantılı hesapta aynı anda izlenemez (İndirilenler'den durdurun)" },
    { "The file is empty", "Dosya boş" }, { "Cannot open the file", "Dosya açılamıyor" },
    /* 1.1: My List and the fast list opening */
    { "My List", "Listem" },
    { "Add to My List", "Listeme ekle" },
    { "Remove from My List", "Listemden çıkar" },
    { "Already in My List", "Zaten Listemde" },
    { "Added to My List", "Listeme eklendi" },
    { "Removed from My List", "Listemden çıkarıldı" },
    { "My List is full", "Listem dolu" },
    { "Could not change My List", "Listem değiştirilemedi" },
    { "My List is empty", "Listem boş" },
    { "Deleted playlists", "Silinmiş oynatma listeleri" },
    { "%d channels from your playlists", "Oynatma listelerinizden %d kanal" },
    { "My List: Select > Add to My List on a channel; Select > Remove from My List in it",
      "Listem: bir kanalda Select > Listeme ekle; Listem içinde Select > Listemden çıkar" },
    { "List updated (%d channels)", "Liste güncellendi (%d kanal)" },
    { "List not checked: %s", "Liste kontrol edilemedi: %s" },
    { "The server's answer is not a channel list", "Sunucunun yanıtı bir kanal listesi değil" },
    { "Could not write the saved copy", "Kayıtlı kopya yazılamadı" },
    /* 0.8.1: PIN, guide, subtitles, theme */
    { "Type the new PIN again", "Yeni PIN'i tekrar yazın" },
    { "The two PINs are not the same: not changed", "İki PIN aynı değil: değiştirilmedi" },
    { "The new PIN is used when you save", "Yeni PIN kaydedince geçerli olur" },
    { "The lock is removed when you save", "Kilit kaydedince kaldırılır" },
    { "No programme guide", "Yayın akışı yok" },
    { "Size", "Boyut" },
    { "Place", "Yer" },
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
    { "Guide", "Reh." }, { "Programme guide", "Yayın rehberi" }, { "Schedule", "Yayın Akışı" }, { "Details", "Detay" }, { "Live", "Canlı" },
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
    { "Time", "Saat" }, { "Day", "Gün" }, { "Choose", "Seç" }, { "Programme", "Program" },
    { "Button", "Düğme" }, { "Previous / next", "Önceki / sonraki" }, { "Season", "Sezon" }, { "Episode", "Bölüm" },
    { "Continue (%s)", "Devam et (%s)" }, { "From the start", "Baştan" }, { "Favourite", "Favori" },
    { "Director:", "Yönetmen:" }, { "Cast:", "Oyuncular:" }, { "Season %d", "Sezon %d" }, { "Episodes", "Bölümler" },
    { "%d season", "%d sezon" }, { "%d seasons", "%d sezon" },
    { "Details and poster come from the provider; without them only the name shows.",
      "Detaylar ve kapak sağlayıcıdan gelir; yoksa yalnız ad görünür." },
    { "Reorder favourites", "Favorileri sırala" }, { "Reorder My List", "Listemi sırala" },
    { "Reorder playlists", "Oynatma listelerini sırala" }, { "Quit Vita IPTV?", "Vita IPTV'den çıkılsın mı?" },
    { "X picks a playlist up, START saves the order", "X ile oynatma listesini tut, START ile sıralamayı kaydet" },
    { "Up / down moves the playlist, X puts it down", "Yukarı / aşağı oynatma listesini taşır, X ile bırak" },
    { "My List stays on top", "Listem hep en üstte kalır" },
    { "Save the settings?", "Ayarlar kaydedilsin mi?" },
    { "M3U playlist", "M3U listesi" }, { "Single stream", "Tek yayın" },
    { "The name in the playlists (empty: made from the address).", "Oynatma listelerinde görünecek ad (boşsa adresten oluşturulur)." },
    { "The address, http:// or https:// (short links work).", "Adres, http:// veya https:// (kısa bağlantılar da olur)." },
    { "The account's username and password.", "Hesabın kullanıcı adı ve şifresi." },
    { "Only if the server or the stream asks for it.", "Yalnızca sunucu veya yayın isterse." },
    { "Removes this playlist (asks first).", "Bu oynatma listesini siler (önce sorar)." }, { "Save the playlist?", "Oynatma listesi kaydedilsin mi?" },
    /* 1.1: the settings' help line */
    { "The language of the app. Automatic follows the console's language.", "Uygulamanın dili. Otomatik, konsolun diline uyar." },
    { "The colours of the app (seen at once).", "Uygulamanın renkleri (hemen görünür)." },
    { "Channels with several picture sizes open at this one.", "Birden fazla görüntü boyutu olan kanallar bu boyutla açılır." },
    { "Hides adult categories and channels. Turning it off asks for the PIN when one is set.",
      "Yetişkin kategorilerini ve kanallarını gizler. PIN varsa kapatırken sorulur." },
    { "Hides the channels with 4K or UHD in their name (the Vita cannot play them).",
      "Adında 4K veya UHD geçen kanalları gizler (Vita bunları oynatamaz)." },
    { "A 4-6 digit PIN: asked before adult channels play and before the lock is changed.",
      "4-6 haneli PIN: yetişkin kanallar açılırken ve kilit değiştirilirken sorulur." },
    { "For quiet channels; a limiter keeps it from distorting. Careful with loud sound on headphones.",
      "Sessiz kanallar için; sınırlayıcı sesin bozulmasını önler. Kulaklıkla yüksek seste dinlerken dikkatli olun." },
    { "For the Vita's speakers: less of the deep bass they cannot play, clearer voices.",
      "Vita hoparlörü için: çalamadığı boğuk bası azaltır, sesleri netleştirir." },
    { "5.1 films and channels: the voices louder in the stereo mix.", "5.1 film ve kanallarda konuşmaları stereo karışımda öne çıkarır." },
    { "Quiet and loud channels come out about as loud as each other (the level follows over a few seconds).",
      "Kısık ve yüksek sesli kanallar yaklaşık aynı seviyede duyulur (seviye birkaç saniyede ayarlanır)." },
    { "Speakers: wider stereo. Headphones: more natural stereo, and 5.1 sound around you (virtual surround). Auto: headphones when they are plugged in.",
      "Hoparlör: daha geniş stereo. Kulaklık: daha doğal stereo, 5.1 seste çevreleyen ses (sanal çevresel ses). Otomatik: kulaklık takılıysa kulaklık." },
    { "Between channels: quiet and loud channels about as loud as each other. Night: also loud scenes down and quiet speech up at once.",   /* 1.2.2 */
      "Kanallar arası: kısık ve yüksek sesli kanallar yaklaşık aynı seviyede duyulur. Gece: ayrıca gürültülü sahneleri anında kısar, kısık konuşmaları yükseltir." },
    { "Speakers: wider stereo, less of the deep bass they cannot play, clearer voices. Headphones: more natural stereo, and 5.1 sound around you. Auto: headphones when they are plugged in.",
      "Hoparlör: daha geniş stereo, çalamadığı boğuk bas azalır, sesler netleşir. Kulaklık: daha doğal stereo, 5.1 seste çevreleyen ses. Otomatik: kulaklık takılıysa kulaklık." },
    { "Plays what the Vita cannot (HEVC, 1080i, AC-3...) through tools/vita_iptv_proxy.py on a PC.",
      "Vita'nın oynatamadığı yayınları (HEVC, 1080i, AC-3...) bilgisayardaki tools/vita_iptv_proxy.py ile oynatır." },
    { "The server's address, e.g. http://192.168.1.20:8090", "Sunucunun adresi, örneğin http://192.168.1.20:8090" },
    { "Checks that the server answers.", "Sunucunun yanıt verip vermediğini dener." },
    { "Removes the recently watched channels of every playlist. Favorites stay.", "Tüm oynatma listelerinin son izlenen kanallarını siler. Favoriler kalır." },
    { "Removes the ticks of watched episodes. Where films stopped is kept.", "İzlenen bölüm işaretlerini siler. Filmlerin kaldığı yer korunur." },
    { "Version, licence and the project's page.", "Sürüm, lisans ve proje sayfası." },
    { "Save the new order?", "Sıralama kaydedilsin mi?" },
    { "X picks a channel up, START saves the order", "X ile kanalı tut, START ile sıralamayı kaydet" },
    { "Up / down moves the channel, X puts it down", "Yukarı / aşağı kanalı taşır, X ile bırak" },
    { "Pick up", "Tut" }, { "Put down", "Bırak" }, { "No memory", "Bellek yetersiz" },
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
    { "Recently Watched", "Son İzlenenler" }, { "Reorder", "Taşı" }, { "Order saved", "Sıralama kaydedildi" },
    { "Order not changed", "Sıralama kaydedilmedi" },
    { "Up / down", "Yukarı / aşağı" },
    { "built", "derleme" },
    { "Now", "Şimdi" }, { "Next", "Sonra" },
    /* 0.7.0 */
    { "Picture", "Görüntü" }, { "Subtitles", "Altyazı" }, { "More", "Diğer" }, { "Forced", "Zorunlu" },
    { "Sync:  < %+d.%d s >", "Senkron:  < %+d.%d sn >" }, { "Sync:  < -0.%d s >", "Senkron:  < -0.%d sn >" },
    { "No subtitles", "Altyazı yok" }, { "Chosen by the server", "Sunucu seçiyor" }, { "No other sound", "Başka ses yok" },
    { "Left / Right: earlier / later   X: back to 0   O: close", "Sol / Sağ: erken / geç   X: sıfırla   O: kapat" },
    { "X: choose   O: close   L / R: tab", "X: seç   O: kapat   L / R: sekme" },
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
    { "Size not known yet", "Boyut henüz belli değil" }, { "Stream", "Yayın" }, { "Delete playlist", "Oynatma listesini sil" },
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
    { "Playlists", "Oynatma listeleri" }, { "%d playlists", "%d liste" },
    { "No playlists yet: press Square to add one", "Henüz liste yok: eklemek için Kare'ye basın" },
    { "Press Square to add your first playlist.", "İlk listenizi eklemek için Kare'ye basın." },
    { "All", "Tümü" }, { "%d channels", "%d kanal" }, { "%d found", "%d sonuç" }, { "Search: \"%s\"", "Arama: \"%s\"" },
    { "Nothing found", "Sonuç yok" }, { "No channels", "Kanal yok" }, { "Search channels", "Kanal ara" },
    { "Loading channels...", "Kanallar yükleniyor..." }, { "Loading channel list...", "Kanal listesi yükleniyor..." },
    { "Checking the account...", "Hesap kontrol ediliyor..." }, { "Loading categories...", "Kategoriler yükleniyor..." },
    { "Cancelling...", "İptal ediliyor..." }, { "Cancelled", "İptal edildi" },
    { "Wrong username or password", "Kullanıcı adı veya şifre yanlış" },
    { "Loading playlists...", "Listeler yükleniyor..." }, { "Starting...", "Başlatılıyor..." },
    { "Add playlist", "Oynatma listesi ekle" }, { "Settings", "Ayarlar" }, { "Reload playlists", "Oynatma listelerini yenile" },
    { "About", "Hakkında" }, { "Quit", "Çıkış" },
    { "Delete this playlist?", "Bu oynatma listesi silinsin mi?" }, { "\"%s\" will be removed from the list.", "\"%s\" listeden kaldırılacak." },
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
    { "Pause", "Dur" }, { "10 s", "10 sn" }, { "5 min", "5 dk" }, { "The end", "Bitti" },
    { "This server does not allow jumping in the file", "Bu sunucu dosyada ileri/geri atlamaya izin vermiyor" },
    { "Jumping is not possible in this file", "Bu dosyada ileri/geri atlanamıyor" },
    { "AVI file: not supported (MKV, MP4, TS and FLV are)", "AVI dosyası: desteklenmiyor (MKV, MP4, TS ve FLV destekleniyor)" },
    { "Unknown file type", "Bilinmeyen dosya türü" },
    /* 1.2.3 */
    { "Download the next episode", "Sonraki bölümü indir" }, { "1 episode", "1 bölüm" }, { "2 episodes", "2 bölüm" },
    { "When 40 percent of an episode is watched, the next ones are downloaded. An account with one connection downloads after you stop watching.",
      "Bölümün yüzde 40'ı izlenince sonraki bölümler indirilir. Tek bağlantılı hesapta indirme, izlemeyi bırakınca başlar." },
    { "Touch screen in the player", "Oynatıcıda dokunmatik" },
    { "Log file", "Kayıt dosyası (log)" },                      /* 1.2.4n */
    { "Everything", "Her şey" },
    { "Errors only", "Yalnız hatalar" },
    { "ux0:data/VitaIPTV/log.txt: only errors (the default), or everything the app does, for finding a problem.",
      "ux0:data/VitaIPTV/log.txt: yalnız hatalar (varsayılan) ya da bir sorunu bulmak için uygulamanın yaptığı her şey." },
    { "In the player: a touch shows or hides the bar, drag the time line to jump, two touches pause.",
      "Oynatıcıda: dokununca çubuk açılır/kapanır, zaman çizgisini sürükleyerek sarılır, iki dokunuş duraklatır." },
    { "Storage", "Depolama" }, { "What the app keeps on the memory card, and clearing it.", "Uygulamanın hafıza kartında tuttukları ve temizliği." },
    { "Next episode queued: it downloads when you stop watching", "Sonraki bölüm sıraya alındı: izlemeyi bırakınca iner" },
    { "Downloading the next episode", "Sonraki bölüm indiriliyor" },
    { "Saved copies of the playlists", "Oynatma listesi kopyaları" }, { "Saved copies of the guides", "Program rehberi kopyaları" },
    { "Unfinished downloads", "Yarım kalan indirmeler" }, { "Logs", "Günlükler" }, { "Archive and search records", "Archive ve arama kayıtları" },
    { "Clear all of these", "Hepsini temizle" }, { "Finished downloads", "Tamamlanan indirmeler" },
    { "They let the lists open at once. Cleared: each list loads from the server the next time.",
      "Listelerin hemen açılmasını sağlar. Temizlenirse her liste bir sonraki açılışta sunucudan yüklenir." },
    { "The programme guides. Cleared: they load again when the guide opens.", "Program rehberleri. Temizlenirse rehber açılınca yeniden yüklenir." },
    { "Downloads that did not finish (stopped or failed). Cleared: they are deleted.", "Bitmemiş (durdurulmuş ya da hata vermiş) indirmeler. Temizlenirse silinir." },
    { "The previous run's log (log_prev.txt). This run's log stays.", "Önceki açılışın günlüğü (log_prev.txt). Bu açılışın günlüğü kalır." },
    { "The archive's chosen files, the last searches, the last web page check.", "Archive'de seçilen dosyalar, son aramalar, son web sayfası denetimi." },
    { "Clears every row above. Favorites, where films stopped, settings and finished downloads stay.",
      "Üstteki satırların hepsini temizler. Favoriler, filmlerin kaldığı yer, ayarlar ve tamamlanan indirmeler kalır." },
    { "Shown only: delete them in Downloads.", "Yalnız gösterilir: İndirilenler'den silinir." },
    { "Cleared: %s freed", "Temizlendi: %s boşaldı" }, { "Finished downloads are deleted in Downloads", "Tamamlanan indirmeler İndirilenler'den silinir" },
    { "none", "yok" }, { "Clear all of these?", "Hepsi temizlensin mi?" }, { "Clear this?", "Temizlensin mi?" },
    /* 1.2.2b */
    { "Not an AVI file", "AVI dosyası değil" }, { "AVI header too big", "AVI başlığı çok büyük" },
    { "AVI header could not be read", "AVI başlığı okunamadı" }, { "AVI file without its header", "AVI dosyasının başlığı yok" },
    { "AVI file without its film (movi)", "AVI dosyasında film yok (movi)" }, { "AVI file without video or sound", "AVI dosyasında görüntü ya da ses yok" },
    { "HLS in parts of one file (byte ranges): not supported", "Tek dosyanın parçalarından oluşan HLS (byte range): desteklenmiyor" },
    { "HLS fMP4: its init segment could not be got", "HLS fMP4: başlangıç parçası alınamadı" },
    { "HLS fMP4: the init segment has no moov", "HLS fMP4: başlangıç parçası bozuk" },
    { "The server does not give this now (HTTP 405): the account is busy or it is not available",
      "Sunucu bunu şu an vermiyor (HTTP 405): hesap meşgul ya da içerik kapalı" },   /* 1.2.2 */
    { "The server sent a web page instead of the film (see log.txt)", "Sunucu film yerine bir web sayfası gönderdi (log.txt'ye bakın)" },   /* 1.2.1 */
    { "The server sent a message instead of the film (see log.txt)", "Sunucu film yerine bir mesaj gönderdi (log.txt'ye bakın)" },
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
    { "Edit playlist", "Oynatma listesini düzenle" }, { "Type", "Tür" }, { "Name", "Ad" }, { "Server address", "Sunucu adresi" },
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
    { "%s could not be reached: switched to %s", "%s kalitesine bağlanılamadı, %s seçildi" }, 
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
    { "The server does not give this one (HTTP %d)", "Sunucu bu içeriği vermiyor (HTTP %s)" },   /* 1.4.1 */
    { "The key of the encrypted stream could not be got (HTTP %d)", "Şifreli yayının anahtarı alınamadı (HTTP %s)" },   /* 1.2.2 */
    { "The first %d of %d films (the most watched)", "%s / %s film (en çok izlenenler)" },   /* 1.5 */
    { "%d of %d films (%s)", "%s / %s film (%s)" },   /* 1.5.1 */
    { "%d videos (%d left out: no MP4 the Vita plays)", "%s video (%s tanesi dışarıda: Vita'nın oynattığı MP4 yok)" },   /* 1.2.1 */
    { "%d videos", "%s video" },
    { "The provider still counts the last stream: %d s", "Sağlayıcı önceki yayını hâlâ sayıyor: %s sn" },   /* 1.2.2 */
    { "%d videos left out (the Vita cannot play them: see webcheck.txt)", "%s video dışarıda kaldı (Vita oynatamaz: webcheck.txt'ye bakın)" },
    { "The page could not be opened (%s)", "Sayfa açılamadı (%s)" },
    { "Server answered HTTP %d (the provider refuses: account busy?)", "Sunucu HTTP %s yanıtı verdi (sağlayıcı reddediyor: hesap kullanımda olabilir)" },
    { "Connection lost (%d times)", "Bağlantı koptu (%s kez)" },
    { "Connection lost (%s)", "Bağlantı koptu (%s)" },                          /* 1.3 (downloads) */
    { "The file cannot be read (at %u MB)", "Dosya okunamıyor (%s MB'ta)" },
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
    { "Encrypted HLS stream (SAMPLE-AES / DRM): not supported", "Şifreli HLS yayını (SAMPLE-AES / DRM): desteklenmiyor" },   /* 1.2.2 */
    { "The key of the encrypted stream could not be got", "Şifreli yayının anahtarı alınamadı" },
    { "HLS with MP4 segments: not supported (MPEG-TS only)", "MP4 parçalı HLS: desteklenmiyor (yalnız MPEG-TS)" },
    { "HLS playlist without a usable stream", "HLS listesinde oynatılabilir yayın yok" },
    { "HLS playlist without segments", "HLS listesinde parça yok" },
    { "HLS playlist inside a playlist: not supported", "İç içe HLS listesi: desteklenmiyor" },
    { "Decoder rejects this stream (0x%08X)", "Çözücü bu yayını kabul etmiyor (0x%s)" },
    { "No memory layout accepted by the decoder (0x%08X)", "Çözücü hiçbir bellek düzenini kabul etmedi (0x%s)" },
    { "Video is %s: not supported", "Görüntü %s: desteklenmiyor" },
    { "Video is %s: %s", "Görüntü %s: %s" },                 /* 1.2.2b (the software decoder could not open) */
    { "Out of memory (%dx%d pictures)", "Bellek yetersiz (%sx%s resim)" },
    { "HLS fMP4: its init segment could not be got (HTTP %d)", "HLS fMP4: başlangıç parçası alınamadı (HTTP %s)" },
    { "H.264 %d-bit, colour %s (the Vita decodes 8-bit 4:2:0 only)", "H.264 %s-bit, renk %s (Vita yalnız 8-bit 4:2:0 çözer)" },
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
