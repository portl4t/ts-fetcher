
#define __STDC_FORMAT_MACROS
#define __STDC_LIMIT_MACROS

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <string>
#include <vector>

#include <ts/ts.h>
#include <ts/experimental.h>
#include <ts/remap.h>

#include <ts_fetcher/ts_fetcher.h>

#define TEST_POST_DATA     "no matter what"

using namespace std;

struct IOHandle {
    TSVIO               vio;
    TSIOBuffer          buffer;
    TSIOBufferReader    reader;

    IOHandle(): vio(NULL), buffer(NULL), reader(NULL) {
    }   

    ~IOHandle() {
        if (reader) {
            TSIOBufferReaderFree(reader);
            reader = NULL;
        }   

        if (buffer) {
            TSIOBufferDestroy(buffer);
            buffer = NULL;
        }   
    }   
};

struct HttpHeader {
    string name;
    string value;
};

class ReqInfo
{
public:
    ReqInfo(): contp(NULL), net_vc(NULL), fch(NULL), send_complete(false), recv_complete(false)
    {
    }

    ~ReqInfo()
    {
        ua_headers.clear();

        if (net_vc) {
            TSVConnClose(net_vc);
            net_vc = NULL;
        }

        if (fch) {
            ts_http_fetcher_destroy(fch);
            fch = NULL;
        }
    }

public:

    TSCont          contp;
    TSVConn         net_vc;
    IOHandle        output;
    IOHandle        input;

    string          url;
    sockaddr        client_addr;
    http_fetcher    *fch;

    vector<HttpHeader>   ua_headers;
    bool            send_complete;
    bool            recv_complete;
};

static int test1_entry(TSCont contp, TSEvent event, void *edata);
static int test1_process(ReqInfo *rinfo, TSVConn conn);
static int test1_handler(TSCont contp, TSEvent event, void *edata);
static void test1_setup_read(ReqInfo *rinfo);
static void test1_fetch_launch(ReqInfo *rinfo);
static int test1_process_read(TSEvent event, ReqInfo *rinfo);
static int test1_process_write(TSEvent event, ReqInfo *rinfo);
static int test1_process_fetch(TSEvent event, ReqInfo *rinfo, void *edata);


TSReturnCode
TSRemapInit(TSRemapInterface *api_info, char *errbuf, int errbuf_size)
{
    if (!api_info)
        return TS_ERROR;

    if (api_info->size < sizeof(TSRemapInterface))
        return TS_ERROR;

    return TS_SUCCESS;
}

TSReturnCode
TSRemapNewInstance(int argc, char* argv[], void** ih, char* errbuf, int errbuf_size)
{
    return TS_SUCCESS;
}

void
TSRemapDeleteInstance(void* ih) 
{
    return;
}

TSRemapStatus
TSRemapDoRemap(void* ih, TSHttpTxn rh, TSRemapRequestInfo *rri)
{
    int         ret;
    const char  *name;
    const char  *value;
    int         name_len, value_len;
    ReqInfo     *rinfo;
    char        *whole_url;
    int         whole_len;
    TSCont      contp;
    TSMLoc      field_loc, next_field_loc;
    HttpHeader  hh;

    ret = TSHttpIsInternalRequest(rh);

    if (!ret) {
        return TSREMAP_NO_REMAP;                // 内部请求就bypass
    } else {
        TSHttpTxnCntl(rh, TS_HTTP_CNTL_SET_LOGGING_MODE, TS_HTTP_CNTL_OFF);
    }

    TSHttpTxnCntl(rh, TS_HTTP_CNTL_SET_LOGGING_MODE, TS_HTTP_CNTL_OFF);
    TSHttpTxnConfigIntSet(rh, TS_CONFIG_HTTP_INSERT_RESPONSE_VIA_STR, 0);

    whole_url = TSUrlStringGet(rri->requestBufp, rri->requestUrl, &whole_len);
    field_loc = TSMimeHdrFieldGet(rri->requestBufp, rri->requestHdrp, 0);

    rinfo = new ReqInfo();

    while (field_loc) {
        name = TSMimeHdrFieldNameGet(rri->requestBufp, rri->requestHdrp, field_loc, &name_len);
        if (name) {
            value = TSMimeHdrFieldValueStringGet(rri->requestBufp, rri->requestHdrp, field_loc, -1, &value_len);

            hh.name = string(name, name_len);
            hh.value = string(value, value_len);
            rinfo->ua_headers.push_back(hh);
        }

        next_field_loc = TSMimeHdrFieldNext(rri->requestBufp, rri->requestHdrp, field_loc);
        TSHandleMLocRelease(rri->requestBufp, rri->requestHdrp, field_loc);
        field_loc = next_field_loc;
    }

    if (field_loc)
        TSHandleMLocRelease(rri->requestBufp, rri->requestHdrp, field_loc);


    rinfo->client_addr = *TSHttpTxnClientAddrGet(rh);
    rinfo->url = string(whole_url, whole_len);

    contp = TSContCreate(test1_entry, TSMutexCreate());

    TSContDataSet(contp, rinfo);

    TSHttpTxnIntercept(contp, rh);

    TSfree(whole_url);
    return TSREMAP_NO_REMAP;
}

static int
test1_entry(TSCont contp, TSEvent event, void *edata)
{
    ReqInfo *rinfo = (ReqInfo*)TSContDataGet(contp);

    switch (event) {

        case TS_EVENT_NET_ACCEPT_FAILED:
            if (edata)
                TSVConnClose((TSVConn)edata);

            delete rinfo;
            TSContDestroy(contp);

            return 0;

        case TS_EVENT_NET_ACCEPT:
            test1_process(rinfo, (TSVConn)edata);
            break;

        default:
            break;
    }

    TSContDestroy(contp);
    return 0;
}

static int
test1_process(ReqInfo *rinfo, TSVConn conn)
{
    TSCont      contp;

    contp = TSContCreate(test1_handler, TSMutexCreate());
    TSContDataSet(contp, rinfo);

    rinfo->contp = contp;
    rinfo->net_vc = conn;

    test1_setup_read(rinfo);
    test1_fetch_launch(rinfo);

    return 0;
}

static void
test1_setup_read(ReqInfo *rinfo)
{
    rinfo->input.buffer = TSIOBufferCreate();
    rinfo->input.reader = TSIOBufferReaderAlloc(rinfo->input.buffer);
    rinfo->input.vio = TSVConnRead(rinfo->net_vc, rinfo->contp, rinfo->input.buffer, INT64_MAX);
}

static void
test1_fetch_launch(ReqInfo *rinfo)
{
    int             n;
    char            buf[32];
    http_fetcher    *fch;

    fch =  ts_http_fetcher_create(rinfo->contp, &rinfo->client_addr, 0);

    ts_http_fetcher_init_common(fch, TS_FETCH_METHOD_POST, rinfo->url.c_str(), rinfo->url.size());

    for (unsigned int i = 0; i < rinfo->ua_headers.size(); i++) {
        ts_http_fetcher_add_header(fch, rinfo->ua_headers[i].name.c_str(), rinfo->ua_headers[i].name.size(),
                            rinfo->ua_headers[i].value.c_str(), rinfo->ua_headers[i].value.size());
    }

    n = sprintf(buf, "%d", (int)(sizeof(TEST_POST_DATA)-1));

    ts_http_fetcher_add_header(fch, TS_MIME_FIELD_CONTENT_LENGTH, TS_MIME_LEN_CONTENT_LENGTH, 
                                    buf, n);

    rinfo->fch = fch;

    ts_http_fetcher_launch(fch);

    ts_http_fetcher_append_data(fch, TEST_POST_DATA, sizeof(TEST_POST_DATA) - 1);
}

static int
test1_handler(TSCont contp, TSEvent event, void *edata)
{
    int     ret = 0;
    ReqInfo *rinfo = (ReqInfo*)TSContDataGet(contp);

    if (edata == rinfo->input.vio) {
        ret = test1_process_read(event, rinfo);

    } else if (edata == rinfo->output.vio){
        ret = test1_process_write(event, rinfo);

    } else {
        ret = test1_process_fetch(event, rinfo, edata);
    }

    if (ret || (rinfo->send_complete && rinfo->recv_complete)) {
        delete rinfo;
        TSContDestroy(contp);
    }

    return 0;
}

static int
test1_process_read(TSEvent event, ReqInfo *rinfo)
{
    switch (event) {

        case TS_EVENT_VCONN_READ_READY:
            TSVConnShutdown(rinfo->net_vc, 1, 0);

        case TS_EVENT_VCONN_READ_COMPLETE:
        case TS_EVENT_VCONN_EOS:
            rinfo->recv_complete = true;
            break;

        default:
            return -1;
    }

    return 0;
}

static int
test1_process_write(TSEvent event, ReqInfo *rinfo)
{
    switch (event) {

        case TS_EVENT_VCONN_WRITE_READY:
            if (TSIOBufferReaderAvail(rinfo->output.reader))
                TSVIOReenable(rinfo->output.vio);
            break;

        case TS_EVENT_VCONN_WRITE_COMPLETE:
            rinfo->send_complete = true;
            break;

        case TS_EVENT_ERROR:
        default:
            return -1; 
    }   

    return 0;
}

static void
test1_setup_write(ReqInfo *rinfo)
{
    rinfo->output.buffer = TSIOBufferCreate();
    rinfo->output.reader = TSIOBufferReaderAlloc(rinfo->output.buffer);
    rinfo->output.vio = TSVConnWrite(rinfo->net_vc, rinfo->contp, rinfo->output.reader, INT64_MAX);

    TSIOBufferCopy(rinfo->output.buffer, rinfo->fch->hdr_reader, TSIOBufferReaderAvail(rinfo->fch->hdr_reader), 0);
    TSVIOReenable(rinfo->output.vio);
}

static int
test1_process_fetch(TSEvent event, ReqInfo *rinfo, void *edata)
{
    int64_t             avail, all; 
    http_fetcher        *fch;

    fch = (http_fetcher*)edata;

    switch (event) {

        case TS_FETCH_EVENT_HEADER_DONE:
            test1_setup_write(rinfo);
            break;

        case TS_FETCH_EVENT_BODY_READY:
        case TS_FETCH_EVENT_BODY_COMPLETE:
            avail = TSIOBufferReaderAvail(fch->body_reader);
            TSIOBufferCopy(rinfo->output.buffer, fch->body_reader, avail, 0);
            ts_http_fetcher_consume_resp_body(fch, avail);

            if (event == TS_FETCH_EVENT_BODY_COMPLETE) {
                all = TSVIONDoneGet(rinfo->output.vio) + TSIOBufferReaderAvail(rinfo->output.reader);
                TSVIONBytesSet(rinfo->output.vio, all);
            }

            if (TSVIONTodoGet(rinfo->output.vio)) {
                if (TSIOBufferReaderAvail(rinfo->output.reader) > 0) {
                    TSVIOReenable(rinfo->output.vio);
                }
            } else {
                rinfo->send_complete = 1;
            }

            break;

        case TS_FETCH_EVENT_ERROR:
        default:
            return -1;
    }

    return 0;
}

