#include "yql_http_gateway.h"

#include <contrib/libs/curl/include/curl/curl.h>
#include <util/stream/str.h>
#include <util/string/builder.h>
#include <util/generic/size_literals.h>

#include <thread>
#include <mutex>
#include <stack>
#include <queue>

namespace NYql {
namespace {

class TEasyCurl {
public:
    using TPtr = std::shared_ptr<TEasyCurl>;
    using TWeakPtr = std::weak_ptr<TEasyCurl>;

    TEasyCurl(const NMonitoring::TDynamicCounters::TCounterPtr&  counter, const NMonitoring::TDynamicCounters::TCounterPtr& downloadedBytes, TString url, IHTTPGateway::THeaders headers, std::size_t offset, std::size_t expectedSize, bool withData)
        : Offset(offset), ExpectedSize(expectedSize), Handle(curl_easy_init()), Counter(counter), DownloadedBytes(downloadedBytes)
    {
        curl_easy_setopt(Handle, CURLOPT_URL, url.c_str());
        curl_easy_setopt(Handle, CURLOPT_POST, withData ?  1L : 0L);
        curl_easy_setopt(Handle, CURLOPT_WRITEFUNCTION, &WriteMemoryCallback);
        curl_easy_setopt(Handle, CURLOPT_WRITEDATA, static_cast<void*>(this));
        curl_easy_setopt(Handle, CURLOPT_USERAGENT, "YQ HTTP gateway");
        curl_easy_setopt(Handle, CURLOPT_SSL_VERIFYPEER, 0L);

        if (!headers.empty()) {
            Headers = std::accumulate(headers.cbegin(), headers.cend(), Headers,
                std::bind(&curl_slist_append, std::placeholders::_1, std::bind(&TString::c_str, std::placeholders::_2)));
            curl_easy_setopt(Handle, CURLOPT_HTTPHEADER, Headers);
        }

        if (Offset) {
            curl_easy_setopt(Handle, CURLOPT_RANGE,  (ToString(Offset) += '-').c_str());
        }

        if (withData) {
            curl_easy_setopt(Handle, CURLOPT_READFUNCTION, &ReadMemoryCallback);
            curl_easy_setopt(Handle, CURLOPT_READDATA, static_cast<void*>(this));
        }
        Counter->Inc();
    }

    virtual ~TEasyCurl() {
        Counter->Dec();
        curl_easy_cleanup(Handle);
        if (Headers) {
            curl_slist_free_all(Headers);
        }
    }

    std::size_t GetExpectedSize() const {
        return ExpectedSize;
    }

    CURL* GetHandle() const {
        return Handle;
    }

    // return true if callback successfully added to this work
    virtual bool AddCallback(IHTTPGateway::TOnResult callback) = 0;
    virtual void Fail(const TIssue& error) = 0;
    virtual void Done(CURLcode result) = 0;

    virtual size_t Write(void* contents, size_t size, size_t nmemb) = 0;
    virtual size_t Read(char *buffer, size_t size, size_t nmemb) = 0;

private:
    static size_t
    WriteMemoryCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        auto self = static_cast<TEasyCurl*>(userp);
        self->DownloadedBytes->Add(size * nmemb);
        return self->Write(contents, size, nmemb);
    };

    static size_t
    ReadMemoryCallback(char *buffer, size_t size, size_t nmemb, void *userp) {
        return static_cast<TEasyCurl*>(userp)->Read(buffer, size, nmemb);
    };

    const std::size_t Offset;
    const std::size_t ExpectedSize;
    CURL *const Handle;
    curl_slist* Headers = nullptr;
    const NMonitoring::TDynamicCounters::TCounterPtr Counter;
    const NMonitoring::TDynamicCounters::TCounterPtr DownloadedBytes;
};

class TEasyCurlBuffer : public TEasyCurl {
public:
    TEasyCurlBuffer(const NMonitoring::TDynamicCounters::TCounterPtr& counter, const NMonitoring::TDynamicCounters::TCounterPtr& downloadedBytes, TString url, TString data, IHTTPGateway::THeaders headers, std::size_t offset, std::size_t expectedSize, IHTTPGateway::TOnResult callback)
        : TEasyCurl(counter, downloadedBytes, url, headers, offset, expectedSize, !data.empty()), Data(std::move(data)), Input(Data), Output(Buffer)
    {
        Output.Reserve(expectedSize);
        Callbacks.emplace(std::move(callback));
    }

    static TPtr Make(const NMonitoring::TDynamicCounters::TCounterPtr&  counter, const NMonitoring::TDynamicCounters::TCounterPtr& downloadedBytes, TString url, TString data, IHTTPGateway::THeaders headers, std::size_t offset, std::size_t expectedSize, IHTTPGateway::TOnResult callback) {
        return std::make_shared<TEasyCurlBuffer>(counter, downloadedBytes, std::move(url), std::move(data), std::move(headers), offset, expectedSize, std::move(callback));
    }
private:
    bool AddCallback(IHTTPGateway::TOnResult callback) final {
        const std::unique_lock lock(SyncCallbacks);
        if (Callbacks.empty())
            return false;
        Callbacks.emplace(std::move(callback));
        return true;
    }

    void Fail(const TIssue& error) final  {
        TIssues issues{error};
        const std::unique_lock lock(SyncCallbacks);
        while (!Callbacks.empty()) {
            Callbacks.top()(issues);
            Callbacks.pop();
        }
    }

    void Done(CURLcode result) final {
        if (CURLE_OK != result)
            return Fail(TIssue(curl_easy_strerror(result)));

        long httpResponseCode = 0;
        curl_easy_getinfo(GetHandle(), CURLINFO_RESPONSE_CODE, &httpResponseCode);

        const std::unique_lock lock(SyncCallbacks);
        while (!Callbacks.empty()) {
            if (1U == Callbacks.size())
                Callbacks.top()(IHTTPGateway::TContent(std::move(Buffer), httpResponseCode));
            else
                Callbacks.top()(IHTTPGateway::TContent(Buffer, httpResponseCode));
            Callbacks.pop();
        }
    }

    size_t  Write(void* contents, size_t size, size_t nmemb) final {
        const auto realsize = size * nmemb;
        Output.Write(contents, realsize);
        return realsize;
    };

    size_t Read(char *buffer, size_t size, size_t nmemb) final {
        return Input.Read(buffer, size * nmemb);
    };

    const TString Data;
    TString Buffer;
    TStringInput Input;
    TStringOutput Output;

    std::mutex SyncCallbacks;
    std::stack<IHTTPGateway::TOnResult> Callbacks;
};

class TEasyCurlStream : public TEasyCurl {
public:
    TEasyCurlStream(const NMonitoring::TDynamicCounters::TCounterPtr&  counter, const NMonitoring::TDynamicCounters::TCounterPtr& downloadedBytes, TString url, IHTTPGateway::THeaders headers, std::size_t offset, std::size_t expectedSize, IHTTPGateway::TOnNewDataPart onNewData, IHTTPGateway::TOnDownloadFinish onFinish)
        : TEasyCurl(counter, downloadedBytes, url, headers, offset, expectedSize, false), OnNewData(std::move(onNewData)), OnFinish(std::move(onFinish))
    {
    }

    static TPtr Make(const NMonitoring::TDynamicCounters::TCounterPtr&  counter, const NMonitoring::TDynamicCounters::TCounterPtr& downloadedBytes, TString url, IHTTPGateway::THeaders headers, std::size_t offset, std::size_t expectedSize, IHTTPGateway::TOnNewDataPart onNewData, IHTTPGateway::TOnDownloadFinish onFinish) {
        return std::make_shared<TEasyCurlStream>(counter, downloadedBytes, std::move(url), std::move(headers), offset, expectedSize, std::move(onNewData), std::move(onFinish));
    }
private:
    bool AddCallback(IHTTPGateway::TOnResult) final { return false; }

    void Fail(const TIssue& error) final  {
        return OnFinish(TIssues{error});
    }

    void Done(CURLcode result) final {
        if (CURLE_OK != result)
            return Fail(TIssue(curl_easy_strerror(result)));

        return OnFinish(std::nullopt);
    }

    size_t  Write(void* contents, size_t size, size_t nmemb) final {
        const auto realsize = size * nmemb;
        OnNewData(IHTTPGateway::TContent(TString(static_cast<char*>(contents), realsize)));
        return realsize;
    };

    size_t Read(char*, size_t, size_t) final { return 0ULL; }

    const IHTTPGateway::TOnNewDataPart OnNewData;
    const IHTTPGateway::TOnDownloadFinish OnFinish;
};

using TKeyType = std::tuple<TString, std::size_t, IHTTPGateway::THeaders, TString, IRetryPolicy<long>::TPtr>;

class TKeyHash {
public:
    TKeyHash() : Hash(), HashPtr() {}

    size_t operator()(const TKeyType& key) const {
        const auto& headers = std::get<2U>(key);
        auto initHash = CombineHashes(CombineHashes(Hash(std::get<0U>(key)), std::get<1U>(key)), Hash(std::get<3U>(key)));
        initHash = CombineHashes(HashPtr(std::get<4U>(key)), initHash);
        return std::accumulate(headers.cbegin(), headers.cend(), initHash,
            [this](size_t hash, const TString& item) { return CombineHashes(hash, Hash(item)); });
    }
private:
    const std::hash<TString> Hash;
    const std::hash<IRetryPolicy<long>::TPtr> HashPtr;
};

class THTTPMultiGateway : public IHTTPGateway {
friend class IHTTPGateway;
public:
    using TPtr = std::shared_ptr<THTTPMultiGateway>;
    using TWeakPtr = std::weak_ptr<THTTPMultiGateway>;

    explicit THTTPMultiGateway(
        const THttpGatewayConfig* httpGatewaysCfg,
        NMonitoring::TDynamicCounterPtr counters)
        : Counters(std::move(counters))
        , Rps(Counters->GetCounter("Requests", true))
        , InFlight(Counters->GetCounter("InFlight"))
        , StraightInFlight(Counters->GetCounter("StraightInFlight"))
        , MaxInFlight(Counters->GetCounter("MaxInFlight"))
        , AllocatedMemory(Counters->GetCounter("AllocatedMemory"))
        , MaxAllocatedMemory(Counters->GetCounter("MaxAllocatedMemory"))
        , OutputMemory(Counters->GetCounter("OutputMemory"))
        , PerformCycles(Counters->GetCounter("PerformCycles", true))
        , AwaitQueue(Counters->GetCounter("AwaitQueue"))
        , AwaitQueueTopExpectedSize(Counters->GetCounter("AwaitQueueTopExpectedSize"))
        , DownloadedBytes(Counters->GetCounter("DownloadedBytes", true))
    {
        if (httpGatewaysCfg) {
            if (httpGatewaysCfg->HasMaxInFlightCount()) {
                MaxHandlers = httpGatewaysCfg->GetMaxInFlightCount();
            }
            MaxInFlight->Set(MaxHandlers);

            if (httpGatewaysCfg->HasMaxSimulatenousDownloadsSize()) {
                MaxSimulatenousDownloadsSize = httpGatewaysCfg->GetMaxSimulatenousDownloadsSize();
            }
            MaxAllocatedMemory->Set(MaxSimulatenousDownloadsSize);
        }

        TaskScheduler.Start();
    }

    ~THTTPMultiGateway() {
        if (Handle)
            curl_multi_wakeup(Handle);
        Thread.join();
    }
private:
    std::size_t MaxHandlers = 1024U;
    std::size_t MaxSimulatenousDownloadsSize = 8_GB;

    static void Perform(const TWeakPtr& weak) {
        OutputSize.store(0ULL);
        curl_global_init(CURL_GLOBAL_ALL);

        if (const auto handle = curl_multi_init()) {
            if (const auto& self = weak.lock()) {
                self->Handle = handle;
            }

            for (size_t handlers = 0U;;) {
                if (const auto& self = weak.lock()) {
                    handlers = self->FillHandlers();
                    self->PerformCycles->Inc();
                    self->OutputMemory->Set(OutputSize);
                } else {
                    break;
                }

                int running = 0;
                if (const auto c = curl_multi_perform(handle, &running); CURLM_OK != c) {
                    if (const auto& self = weak.lock()) {
                        self->Fail(c);
                    }
                    break;
                }

                if (running < int(handlers)) {
                    if (const auto& self = weak.lock()) {
                        for (int messages = int(handlers) - running; messages;) {
                            if (const auto msg = curl_multi_info_read(handle, &messages)) {
                                if(msg->msg == CURLMSG_DONE) {
                                    self->Done(msg->easy_handle, msg->data.result);
                                }
                            }
                        }
                    }
                } else {
                    if (const auto c = curl_multi_poll(handle, nullptr, 0, 1024, nullptr); CURLM_OK != c) {
                        if (const auto& self = weak.lock()) {
                            self->Fail(c);
                        }
                        break;
                    }
                }
            }

            curl_multi_cleanup(handle);
        }

        curl_global_cleanup();
    }

    size_t FillHandlers() {
        const std::unique_lock lock(Sync);
        const ui64 topExpectedSize = Await.empty() ? 0 : Await.front()->GetExpectedSize(); 
        AwaitQueueTopExpectedSize->Set(topExpectedSize);
        while (!Await.empty() && Allocated.size() < MaxHandlers && AllocatedSize + Await.front()->GetExpectedSize() <= MaxSimulatenousDownloadsSize) {
            AllocatedSize += Await.front()->GetExpectedSize();
            const auto handle = Await.front()->GetHandle();
            Allocated.emplace(handle, std::move(Await.front()));
            Await.pop();
            curl_multi_add_handle(Handle, handle);
        }
        AwaitQueue->Set(Await.size());
        AllocatedMemory->Set(AllocatedSize);
        return Allocated.size();
    }

    void Done(CURL* handle, CURLcode result) {
        TEasyCurl::TPtr easy;
        bool isRetry = false;
        {
            const std::unique_lock lock(Sync);
            if (const auto it = Allocated.find(handle); Allocated.cend() != it) {
                long httpResponseCode = 0;
                easy = std::move(it->second);
                curl_easy_getinfo(easy->GetHandle(), CURLINFO_RESPONSE_CODE, &httpResponseCode);

                if (const auto stateIt = Easy2RetryState.find(easy); stateIt != Easy2RetryState.end()) {
                    if (const auto& nextRetryDelay = stateIt->second->GetNextRetryDelay(httpResponseCode)) {
                        Y_VERIFY(isRetry = TaskScheduler.Add(new THttpGatewayTask(easy, Singleton), *nextRetryDelay));
                    } else {
                        Easy2RetryState.erase(stateIt);
                    }
                }
                AllocatedSize -= easy->GetExpectedSize();
                Allocated.erase(it);
            }

            if (Await.empty() && Allocated.empty())
                Requests.clear();
        }
        if (!isRetry && easy) {
            easy->Done(result);
        }
    }

    void Fail(CURLMcode result) {
        std::stack<TEasyCurl::TPtr> works;
        {
            const std::unique_lock lock(Sync);

            for (const auto& item : Allocated) {
                works.emplace(std::move(item.second));
                AllocatedSize -= works.top()->GetExpectedSize();
            }

            AllocatedSize = 0ULL;
            Allocated.clear();
            if (Await.empty())
                Requests.clear();
        }

        const TIssue error(curl_multi_strerror(result));
        while (!works.empty()) {
            works.top()->Fail(error);
            works.pop();
        }
    }

    void Download(
        TString url,
        THeaders headers,
        std::size_t expectedSize,
        TOnResult callback,
        TString data,
        IRetryPolicy<long>::TPtr retryPolicy) final
    {
        Rps->Inc();

        if (expectedSize > MaxSimulatenousDownloadsSize) {
            TIssue error(TStringBuilder() << "Too big file for downloading: size " << expectedSize << ", but limit is " << MaxSimulatenousDownloadsSize);
            callback(TIssues{error});
            return;
        }
        const std::unique_lock lock(Sync);
        auto& entry = Requests[TKeyType(url, 0U, headers, data, retryPolicy)];
        StraightInFlight->Set(Requests.size());
        if (const auto& easy = entry.lock())
            if (easy->AddCallback(callback))
                return;

        auto easy = TEasyCurlBuffer::Make(InFlight, DownloadedBytes, std::move(url), std::move(data), std::move(headers), 0U, expectedSize, std::move(callback));
        entry = easy;
        Easy2RetryState.emplace(easy, std::move(retryPolicy->CreateRetryState()));
        Await.emplace(std::move(easy));
        Wakeup(expectedSize);
    }

    void Download(
        TString url,
        THeaders headers,
        std::size_t offset,
        TOnNewDataPart onNewData,
        TOnDownloadFinish onFinish) final
    {
        constexpr auto buffersSize = CURL_MAX_WRITE_SIZE << 4U;
        auto easy = TEasyCurlStream::Make(InFlight, DownloadedBytes, std::move(url), std::move(headers), offset, buffersSize, std::move(onNewData), std::move(onFinish));
        const std::unique_lock lock(Sync);
        Await.emplace(std::move(easy));
        Wakeup(buffersSize);
    }

    void OnRetry(TEasyCurl::TPtr easy) {
        const std::unique_lock lock(Sync);
        const std::size_t expectedSize = easy->GetExpectedSize();
        Await.emplace(std::move(easy));
        Wakeup(expectedSize);
    }

    void Wakeup(std::size_t expectedSize) {
        AwaitQueue->Set(Await.size());
        if (Allocated.size() < MaxHandlers && AllocatedSize + expectedSize + OutputSize.load() <= MaxSimulatenousDownloadsSize) {
            curl_multi_wakeup(Handle);
        }
    }

    class THttpGatewayTask: public TTaskScheduler::IRepeatedTask {
    public:
        THttpGatewayTask(
            TEasyCurl::TPtr easy,
            THTTPMultiGateway::TWeakPtr gateway)
            : Easy(easy)
            , Gateway(gateway)
        {}

        bool Process() override {
            if (const auto g = Gateway.lock()) {
                Y_VERIFY(Easy);
                g->OnRetry(std::move(Easy));
            }
            return false;
        }
    private:
        TEasyCurl::TPtr Easy;
        THTTPMultiGateway::TWeakPtr Gateway;
    };

private:
    CURLM* Handle = nullptr;

    std::queue<TEasyCurl::TPtr> Await;

    std::unordered_map<CURL*, TEasyCurl::TPtr> Allocated;
    std::unordered_map<TKeyType, TEasyCurl::TWeakPtr, TKeyHash> Requests;
    std::unordered_map<TEasyCurl::TPtr, IRetryPolicy<long>::IRetryState::TPtr> Easy2RetryState;

    std::mutex Sync;
    std::thread Thread;

    std::size_t AllocatedSize = 0ULL;
    static std::atomic_size_t OutputSize;

    static std::mutex CreateSync;
    static TWeakPtr Singleton;

    const NMonitoring::TDynamicCounterPtr Counters;
    const NMonitoring::TDynamicCounters::TCounterPtr Rps;
    const NMonitoring::TDynamicCounters::TCounterPtr InFlight;
    const NMonitoring::TDynamicCounters::TCounterPtr StraightInFlight; // doesn't consider merged requests which use one curl
    const NMonitoring::TDynamicCounters::TCounterPtr MaxInFlight;
    const NMonitoring::TDynamicCounters::TCounterPtr AllocatedMemory;
    const NMonitoring::TDynamicCounters::TCounterPtr MaxAllocatedMemory;
    const NMonitoring::TDynamicCounters::TCounterPtr OutputMemory;
    const NMonitoring::TDynamicCounters::TCounterPtr PerformCycles;
    const NMonitoring::TDynamicCounters::TCounterPtr AwaitQueue;
    const NMonitoring::TDynamicCounters::TCounterPtr AwaitQueueTopExpectedSize;
    const NMonitoring::TDynamicCounters::TCounterPtr DownloadedBytes;

    TTaskScheduler TaskScheduler;
};

std::atomic_size_t THTTPMultiGateway::OutputSize = 0ULL;
std::mutex THTTPMultiGateway::CreateSync;
THTTPMultiGateway::TWeakPtr THTTPMultiGateway::Singleton;

}

IHTTPGateway::TContent::TContent(TString&& data, long httpResponseCode)
    : TString(std::move(data))
    , HttpResponseCode(httpResponseCode)
{
    if (!empty()) {
        THTTPMultiGateway::OutputSize.fetch_add(size());
    }
}

IHTTPGateway::TContent::TContent(const TString& data, long httpResponseCode)
    : TString(data)
    , HttpResponseCode(httpResponseCode)
{
    if (!empty()) {
        THTTPMultiGateway::OutputSize.fetch_add(size());
    }
}

IHTTPGateway::TContent::TContent(TString&& data)
    : TContent(std::move(data), 0)
{}

IHTTPGateway::TContent::TContent(const TString& data)
    : TContent(data, 0)
{}

TString IHTTPGateway::TContent::Extract() {
    if (!empty()) {
        THTTPMultiGateway::OutputSize.fetch_sub(size());
    }
    return std::move(*this);
}

IHTTPGateway::TContent::~TContent()
{
    if (!empty()) {
        THTTPMultiGateway::OutputSize.fetch_sub(size());
    }
}

IHTTPGateway::TPtr
IHTTPGateway::Make(const THttpGatewayConfig* httpGatewaysCfg, NMonitoring::TDynamicCounterPtr counters) {
    const std::unique_lock lock(THTTPMultiGateway::CreateSync);
    if (const auto g = THTTPMultiGateway::Singleton.lock())
        return g;

    const auto gateway = std::make_shared<THTTPMultiGateway>(httpGatewaysCfg, std::move(counters));
    THTTPMultiGateway::Singleton = gateway;
    gateway->Thread = std::thread(std::bind(&THTTPMultiGateway::Perform, THTTPMultiGateway::Singleton));
    return gateway;
}


}
