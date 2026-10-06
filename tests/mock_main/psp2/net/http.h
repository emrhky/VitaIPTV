#ifndef MOCK_HTTP_H
#define MOCK_HTTP_H
enum { SCE_HTTP_METHOD_GET, SCE_HTTP_METHOD_POST, SCE_HTTP_METHOD_HEAD };
enum { SCE_HTTP_VERSION_1_0 = 1, SCE_HTTP_VERSION_1_1 };
enum { SCE_HTTP_HEADER_OVERWRITE, SCE_HTTP_HEADER_ADD };
int sceHttpCreateTemplate(const char *ua, int ver, int keep);
int sceHttpDeleteTemplate(int id);
int sceHttpSetResolveTimeOut(int id, unsigned usec);
int sceHttpSetConnectTimeOut(int id, unsigned usec);
int sceHttpSetRecvTimeOut(int id, unsigned usec);
int sceHttpSetAutoRedirect(int id, int enable);
int sceHttpCreateConnectionWithURL(int tmpl, const char *url, int keep);
int sceHttpDeleteConnection(int id);
int sceHttpCreateRequestWithURL(int conn, int method, const char *url, unsigned long long cl);
int sceHttpDeleteRequest(int id);
int sceHttpAddRequestHeader(int id, const char *name, const char *value, unsigned mode);
int sceHttpSendRequest(int req, const void *post, unsigned size);
int sceHttpGetStatusCode(int req, int *status);
int sceHttpGetResponseContentLength(int req, unsigned long long *len);
int sceHttpReadData(int req, void *data, unsigned size);
int sceHttpInit(unsigned pool); int sceHttpTerm(void);
int sceHttpAbortRequest(int req);
#endif
