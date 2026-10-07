#include "lang.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int g_lang = LANG_EN;
void lang_set(int lang) { g_lang = lang == LANG_TR ? LANG_TR : LANG_EN; }
int  lang_get(void) { return g_lang; }

typedef struct { const char *en, *tr; } Pair;

static const Pair FIXED[] = {
    /* buttons */
    { "Open", "Aç" }, { "Add", "Ekle" }, { "Edit", "Düzenle" }, { "Delete", "Sil" }, { "Menu", "Menü" },
    { "Play", "Oynat" }, { "Search", "Ara" }, { "Group", "Grup" }, { "Page", "Sayfa" }, { "Back", "Geri" },
    { "New search", "Yeni arama" }, { "Clear", "Temizle" }, { "Channel", "Kanal" }, { "Info", "Bilgi" },
    { "Server", "Sunucu" }, { "Analyze", "Analiz" }, { "Station", "İstasyon" }, { "Again", "Tekrar" },
    { "Move", "Gezin" }, { "Edit / choose", "Düzenle / seç" }, { "Cancel", "İptal" }, { "Screen off", "Ekranı kapat" },
    { "Yes", "Evet" }, { "No", "Hayır" }, { "Category", "Kategori" }, { "Categories", "Kategoriler" },
    { "Channels", "Kanallar" },
    /* playlists and channels */
    { "Playlists", "Listeler" }, { "%d playlists", "%d liste" },
    { "No playlists yet: press Square to add one", "Henüz liste yok: eklemek için Kare'ye basın" },
    { "Press Square to add your first playlist.", "İlk listenizi eklemek için Kare'ye basın." },
    { "All", "Tümü" }, { "%d channels", "%d kanal" }, { "%d found", "%d sonuç" }, { "Search: \"%s\"", "Arama: \"%s\"" },
    { "Nothing found", "Sonuç yok" }, { "No channels", "Kanal yok" }, { "Search channels", "Kanal ara" },
    { "Loading channels...", "Kanallar yükleniyor..." }, { "Loading channel list...", "Kanal listesi yükleniyor..." },
    { "Loading playlists...", "Listeler yükleniyor..." }, { "Starting...", "Başlatılıyor..." },
    { "Add playlist", "Liste ekle" }, { "Settings", "Ayarlar" }, { "Reload sources", "Listeleri yenile" },
    { "About", "Hakkında" }, { "Quit", "Çıkış" },
    { "Delete this playlist?", "Bu liste silinsin mi?" }, { "\"%s\" will be removed from the list.", "\"%s\" listeden kaldırılacak." },
    { "Deleted", "Silindi" }, { "Saved", "Kaydedildi" }, { "Settings saved", "Ayarlar kaydedildi" },
    { "Saved as Xtream (faster, live channels only)", "Xtream olarak kaydedildi (daha hızlı, yalnızca canlı kanallar)" },
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
    { "Server: plays 1080p, HEVC, MKV, MP2/AC-3 (tools/vita_iptv_proxy.py)",
      "Sunucu: 1080p, HEVC, MKV, MP2/AC-3 oynatır (tools/vita_iptv_proxy.py)" },
    { "Enter the server address first", "Önce sunucu adresini girin" }, { "Bad server address", "Geçersiz sunucu adresi" },
    { "Testing the server...", "Sunucu deneniyor..." }, { "Server is running", "Sunucu çalışıyor" },
    { "No answer from the server: %s", "Sunucudan yanıt yok: %s" }, { "unexpected reply", "beklenmeyen yanıt" },
    { "Set a transcoding server in START > Settings", "START > Ayarlar'dan bir dönüştürme sunucusu girin" },
    { "Language", "Dil" }, { "Automatic (system)", "Otomatik (sistem)" }, { "1080p module", "1080p modülü" }, { "Hide adult channels", "Yetişkin kanallarını gizle" }, { "1080p decoder (experimental)", "1080p çözücü (deneysel)" },
    { "loaded", "yüklendi" }, { "not found", "bulunamadı" },
    /* player */
    { "Connecting...", "Bağlanıyor..." }, { "Waiting for the stream", "Yayın bekleniyor" },
    { "Starting video...", "Görüntü başlatılıyor..." }, { "Waiting for a keyframe", "Anahtar kare bekleniyor" },
    { "Stream ended", "Yayın bitti" }, { "Cannot play this channel", "Bu kanal oynatılamıyor" }, { "Cannot start", "Başlatılamadı" },
    { "Cannot play this file", "Bu dosya oynatılamıyor" }, { "No video after 20 s", "20 sn içinde görüntü gelmedi" },
    { "See ux0:data/VitaIPTV/log.txt", "Ayrıntı: ux0:data/VitaIPTV/log.txt" },
    { "Hide 1080p channels", "1080p kanalları gizle" },
    { "Tip: a transcoding server on your PC can play this channel (START > Settings, see README)",
      "İpucu: bilgisayarınızdaki dönüştürme sunucusu bu kanalı oynatabilir (START > Ayarlar, README)" },
    { "Tip: put reAvPlayer.suprx in ux0:data/VitaIPTV/ for 1080p, or use the transcoding server",
      "İpucu: 1080p için reAvPlayer.suprx dosyasını ux0:data/VitaIPTV/ içine koyun ya da dönüştürme sunucusunu kullanın" },
    { "The 1080p module is loaded, but the decoder still refused 1080p", "1080p modülü yüklü ama çözücü 1080p'yi yine reddetti" },
    { "Played through the transcoding server. Is it running? (START > Settings > Test)",
      "Dönüştürme sunucusu üzerinden oynatıldı. Sunucu çalışıyor mu? (START > Ayarlar > Sunucuyu dene)" },
    { "No sound: press Square to play through the server", "Ses yok: sunucu üzerinden oynatmak için Kare'ye basın" },
    { "via server", "sunucu üzerinden" }, { "  via server", "  sunucu üzerinden" },
    { "Video %dx%d   shown %u   dropped %u   late %u   damaged %u   errors %u   %u KB",
      "Görüntü %dx%d   gösterilen %u   atlanan %u   geç %u   hasarlı %u   hata %u   %u KB" },
    { "Audio AAC %s %s   A/V %+d ms", "Ses AAC %s %s   A/V %+d ms" }, { "Audio AAC %s %s", "Ses AAC %s %s" },
    { "Audio: waiting", "Ses: bekleniyor" },
    { "Radio", "Radyo" }, { "ON AIR", "YAYINDA" }, { "Buffering...", "Arabelleğe alınıyor..." },
    { "START turns the screen off; the radio keeps playing.", "START ekranı kapatır; radyo çalmaya devam eder." },
    { "Matroska (MKV)", "Matroska (MKV)" },
    { "Stream analysis", "Yayın analizi" }, { "Analyzing stream...", "Yayın analiz ediliyor..." },
    { "Cannot start analysis", "Analiz başlatılamadı" }, { "Cannot start the TS player", "TS oynatıcı başlatılamadı" },
    { "Press O to close", "Kapatmak için O" }, { "Live TV and radio on your Vita", "Vita'nızda canlı TV ve radyo" },
    { "Live TV (MPEG-TS, H.264 up to 720p, AAC) and radio.", "Canlı TV (MPEG-TS, 720p'ye kadar H.264, AAC) ve radyo." },
    { "Other formats via the transcoding server (see README).", "Diğer biçimler dönüştürme sunucusuyla (README)." },
    /* messages without numbers */
    { "HEVC (H.265) video: the Vita cannot decode it", "HEVC (H.265) görüntü: Vita bunu çözemiyor" },
    { "This channel sends MKV video, not MPEG-TS", "Bu kanal MPEG-TS değil, MKV görüntü gönderiyor" },
    { "HLS playlist (.m3u8): not supported", "HLS listesi (.m3u8): desteklenmiyor" },
    { "HLS playlist (.m3u8): not supported yet", "HLS listesi (.m3u8): henüz desteklenmiyor" },
    { "The server sent a web page instead of video", "Sunucu görüntü yerine bir web sayfası gönderdi" },
    { "Server sent a web page, not video", "Sunucu görüntü yerine bir web sayfası gönderdi" },
    { "No MPEG-TS data (not a TS stream?)", "MPEG-TS verisi yok (TS yayını değil mi?)" },
    { "No MPEG-TS data found (not a TS stream?)", "MPEG-TS verisi bulunamadı (TS yayını değil mi?)" },
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
    { "%dx%d is above the Vita decoder limit (720p)", "%sx%s, Vita çözücüsünün sınırının (720p) üstünde" },
    { "Server answered HTTP %d", "Sunucu HTTP %s yanıtı verdi" },
    { "Connection failed (0x%08X)", "Bağlantı kurulamadı (0x%s)" },
    { "HTTPS failed (0x%08X): this site needs newer TLS than the Vita has. Copy the .m3u file to ux0:data/VitaIPTV/ instead",
      "HTTPS basarisiz (0x%s): bu site Vita'dakinden yeni TLS istiyor. .m3u dosyasini PC'den ux0:data/VitaIPTV/ klasorune kopyalayin" },
    { "Bad address (0x%08X)", "Geçersiz adres (0x%s)" }, { "Request failed (0x%08X)", "İstek başarısız (0x%s)" },
    { "Read error (0x%08X)", "Okuma hatası (0x%s)" }, { "HTTP init failed (0x%08X)", "HTTP başlatılamadı (0x%s)" },
    { "Stream stopped (connection lost %d times)", "Yayın durdu (bağlantı %s kez koptu)" },
    { "No usable keyframe in %u pictures", "%s karede kullanılabilir anahtar kare yok" },
    { "Decoder produced no picture (%u errors). Switch the Vita fully off and on again, then try a 720p channel",
      "Çözücü görüntü üretmedi (%s hata). Vita'yı tamamen kapatıp açın, sonra 720p kanalı deneyin" },
    { "Decoder init failed (0x%08X), see log.txt", "Çözücü başlatılamadı (0x%s), log.txt'ye bakın" },
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
