/* 小缓冲区护栏: scheme-relative URL 的 http: 前缀也必须受 out_len 限制。 */
#ifndef NETCLIENTCURL_SOURCE
#define NETCLIENTCURL_SOURCE "../src/clients/net/NetClientCurl.c"
#endif
#include NETCLIENTCURL_SOURCE

#include <assert.h>

int main(void)
{
    struct {
        char out[16];
        unsigned char guard[16];
    } buf;
    memset(&buf, 0xa5, sizeof(buf));
    resolve_url(buf.out, sizeof(buf.out), "host/path", "//012345678901234567890123456789");
    assert(buf.out[sizeof(buf.out) - 1] == '\0');
    for (size_t i = 0; i < sizeof(buf.guard); i++) assert(buf.guard[i] == 0xa5);
    assert(strcmp(buf.out, "http://01234567") == 0);
    char out[128];
    resolve_url(out, sizeof(out), "https://host/path", "//cdn/item");
    assert(strcmp(out, "https://cdn/item") == 0);
    resolve_url(out, sizeof(out), "https://host/a/file?q=1", "next");
    assert(strcmp(out, "https://host/a/next") == 0);
    resolve_url(out, sizeof(out), "https://host/a/file", "/next");
    assert(strcmp(out, "https://host/next") == 0);
    puts("URL缓冲区与重定向回归: 通过");
    return 0;
}
