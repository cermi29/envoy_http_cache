# Envoy with a Simple Http Cache

## Design
Each HEAD or GET request gets a check against the cache. If it is a new entry, a new request is created with the same headers to populate the cache (converted to GET with no range) and all new incoming requests for the same entry get put into a queue of schedulable callbacks, including the first original request. This queue is then scheduled for execution everytime some new data arrives. If the request is deemed not cacheable all requests waiting in the queue are recreated and then bypass the cache along with any new incoming requests for that entry.

Cache is implemented as a vector of cache entries. Each entry contains a pointer to a potential new entry to form a simple list. Therefore, in case of a cache collision, each index can hold an unspecified amount of extra entries.

New incoming data is stored in a list of vectors to easily allow concurrent reading and writing. No mutex held while writing or reading new data.

Cache key is set as host + path. Cache size is configurable via a `http_cache_size` entry in `static_resources` in config. Cache is accessed as a `ThreadSafeSingleton`.

## Watermark Buffers
I didn't really dive deep into this as I wanted to send this sooner. If I understood it correctly watermarking is so that buffers don't get filled faster than they are spent which both lowers memory used and balances out network usage. 

Watermarking is currently not implemented. My initial guess as to how to make it work would be using the low watermark callback to either immediately fill the buffer with a new fragment or schedule a callback to do so. Would also require some changes to how I keep track of data sent, currently I keep track of the number of fragments sent, would be better to keep tracks of bytes of data sent and add just 1 chunk of a fragment to the buffer at a time.

## Limitations
- Ignores `Range` headers.
  - A small alteration to how the data is stored and read would have to be done. Request objects would have to keep track of a start and an end (currently just keeps track of how many fragments of the available data have been sent), `data_num_` would keep track of bytes written instead of the number of fragments available. And some corresponding logic to only send certain bytes in the `reply()` function, but that could also change based on how the watermark implementation would work.
- Ignores request's directives to bypass cache (or verify the entry).
  - Could be solved by a check for these at the start of `search()` function and either letting them bypass the cache or put them in a waiting queue of some sort until the entry gets revalidated.
- No popping of entries from the cache due to invalidation.
  - Example solution, if a new request comes in for an entry that surpassed its `max-age`. We invalidate the current data and start a new request, put incoming requests into the queue as if it was a new entry. Though we should probably keep the old data until all pending requests using it are done, for example, we could keep track of current readers via the lambda used in `BufferFragmentImpl` in `reply()` function. I believe that other causes for invalidation could be handled in the same or similar matter.
- Ignores Vary headers.
  - A solution would be to have a basic entry for the URL tell us to use a new hash based on the vary headers combined with the URL. Then new entries could be created and searched in the same cache based on the new hash and with new keys including the vary headers. I suppose in some cases, like encoding, the cache could also potentially handle it directly.
- The only cache control directives recognized are `no-cache`, `no-store`, `private` and `Vary: *`.
- No alteration of response headers for individual clients.
- No flow control since watermarking doesn't work.

## Other Possible Improvements
- Better working with mutexes. I prefered to stay on the safe side using 1 mutex per entry. With proper planning, utilizing multiple mutexes and reader/writer locks, the time threads spend blocked could probably get significantly reduced.
- After the initial herd gets processed, individual slices of data could be serialized to ease access.
- The `http_cache_size` configuration should be somewhere else than in `static_resources`.
- Some things could be named better.
- And many other things I can't even think of right now.

## Relevant Files
New files:
- [router_http_cache.cc](https://github.com/cermi29/envoy_http_cache/tree/main/source/common/router/router_http_cache.cc) New file, contains all the logic behind the cache.
- [router_http_cache.h](https://github.com/cermi29/envoy_http_cache/tree/main/source/common/router/router_http_cache.h) New header file for the cache.

Altered files:
- [router.cc](https://github.com/cermi29/envoy_http_cache/tree/main/source/common/router/router.cc) added a cache search in` decodeHeaders()` that potentially lets the cache handle the request. Also in `onDestroy()` removes event callback from the cache event list.
- [router.h](https://github.com/cermi29/envoy_http_cache/tree/main/source/common/router/router.h) added a variable to store the cache related request 
- [server.cc](https://github.com/cermi29/envoy_http_cache/tree/main/source/server/server.cc) added init cache with size from config
- [bootstrap.proto](https://github.com/cermi29/envoy_http_cache/tree/main/api/envoy/config/bootstrap/v3/bootstrap.proto) added `http_cache_size` entry to `static_resources`

## Building
Built with clang version 14.0.6, using `--config=clang` as per the recommendation in [readme](https://github.com/cermi29/envoy_http_cache/blob/main/bazel/README.md).

## Testing Done
I did some basic testing using a local server that replies with the request path, simulating delay before response. Tested a herd for 1 URL, multiple URLs, small delay, big delay, `no-cache` directive, with normal cache size or size 1 to see how it behaves with a lot of collisions. I've only been using a very basic [config](https://github.com/cermi29/envoy_http_cache/blob/main/configs/demo.yaml "config").

## Time invested
A very rough estimate:
- 30 hours studying envoy docs, codebase, RFC 9111 etc.
- 25 hours programming + also further studying envoy code
- Almost no debugging, the only issue I remember having was deleting a schedulable callback's object prematurely, which took me a few minutes to figure out.

I've spent quite a bit of time just trying to find the right place to put my cache in the codebase. Router was my first guess almost right at the start but I dismissed it for some reason that I don't even clearly remember, I think I got put off by how long the `decodeHeaders()` function was. Then I kept going around in circles until I finally landed back in router, but at least I learned a bit extra about the inner workings of envoy, including the `AsyncClient` for shadowing requests that I ended up using to create a new request to populate the cache entry, so that I don't have to rely on the original request not being cancelled, as noted [here](https://github.com/cermi29/envoy_http_cache/blob/main/source/extensions/http/cache/file_system_http_cache/DESIGN.md#thundering-herd). Though it still ends up going through all the filters again.
