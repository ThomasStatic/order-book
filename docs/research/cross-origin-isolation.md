# Cross-origin isolation for `/order-book` on Vercel

Research for [issue #111](https://github.com/ThomasStatic/order-book/issues/111) on the wayfinder map, [#109](https://github.com/ThomasStatic/order-book/issues/109). Read 2026-11-09.

The question: how can `/order-book` on the website be cross-origin isolated, so that `SharedArrayBuffer` and Emscripten pthreads work, on Vercel's free (Hobby) plan, and what does that cost the rest of the site? The website was read at commit `e003300` of the local checkout (`C:\dev\Thomas-Boyle-Website`), and the live site was checked with `curl` on the same day. The decision itself belongs to [#120](https://github.com/ThomasStatic/order-book/issues/120), "How the website loads and ships the engine". This note ends with a recommendation for it.

Citations are reference links (for example [mdn-coep]), resolved in [Sources](#sources). "Observed" means a `curl` against the live site or a third-party host on 2026-11-09, not documentation. "Not stated" means the pages read say nothing about it. Nothing is filled in from memory.

## The answer in brief

- **Vercel serves this site as static files. `server.ts` never runs.** Vercel's Angular preset uses `dist/<project>/browser` as the output directory, and its only routes are "serve the file if it exists", then `/(.*)` → `/index.html` [vercel-preset]. The live site behaves that way: `/order-book` comes back as a static file, and an unknown path comes back as `index.html` (observed). So every response is either a prerendered file or the SPA fallback. No SSR responses exist, so the question of whether SSR and prerendered responses both get the headers doesn't arise. The one way to set COOP/COEP is `vercel.json` `headers`, which Vercel documents as applying to static files and functions alike [vercel-json-headers]. Angular's own per-route `headers` [ng-ssr] [ng-prerender-route] would need the Angular server to be running, and it isn't.
- **Isolation belongs to the document, so client-side routing defeats a per-route header.** A browsing context group (the set of windows and frames that can script each other) gets its cross-origin isolation mode only when a top-level navigation creates it with COOP `same-origin` plus a compatible COEP [html-coop-coep] [html-obtain-bc]. A window can use isolated APIs only if its agent cluster's mode is `"concrete"` [html-window-coi]. An Angular router navigation from `/` to `/order-book` creates no new document, so the page stays non-isolated. The site has three such `routerLink`s into `/order-book` (see [§3](#3-isolation-is-per-document-what-client-side-routing-does)).
- **`require-corp` costs this site nothing today, and it is the only COEP value that works in every engine.** `credentialless` is in Chrome 96 and Firefox 119, but not in Safari, iOS, or Firefox for Android [bcd-coep]. The only cross-origin subresources the site loads are Google Fonts (CSS and font files), and both already send `Cross-Origin-Resource-Policy: cross-origin` and `Access-Control-Allow-Origin: *` (observed). Vercel Web Analytics is same-origin. The site has no iframes, no cross-origin images, and no use of `window.opener`.
- **An isolated iframe can't live under a non-isolated parent** in standard HTML. A child frame joins its top-level browsing context group [html-child-navigable], and that group's mode is `"none"` unless the top-level document set COOP and COEP [html-coi-mode]. Chrome's `Document-Isolation-Policy` lifts this, but only in Chrome (137 on desktop, 146 on Android) [bcd-dip] [chrome-dip].
- **Recommendation:** isolate the whole site. Send `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp` on `/(.*)` from a `vercel.json`, and give the engine's assets an explicit `Content-Type` and a long cache lifetime. That removes the routing problem rather than working around it, and it also covers the engine's worker scripts, which need COEP of their own [html-worker-coep]. Its cost is a rule for the future: every cross-origin embed must send CORP or CORS. Details and the fallback option are in [§7](#7-recommendation).

## 1. What the site loads today

Read from `src/index.html`, `angular.json`, `server.ts`, `src/app/**`, and `public/` at `e003300`.

| What | Where from | Mode | Effect of COEP `require-corp` |
|---|---|---|---|
| App bundles, chunks, styles | Same origin (`main-*.js`, `chunk-*.js`) | Same-origin | None |
| Images (`public/*.png`, `*.jpg`, `playing-cards/`, `cursors/`) | Same origin; no `<img>` in `src/app` points at another host | Same-origin | None |
| Google Fonts CSS: `fonts.googleapis.com/css2?family=VT323`, `Roboto`, `icon?family=Material+Icons` | `<link rel="stylesheet">` in `src/index.html`, no `crossorigin` attribute | `no-cors` | Needs CORP. It sends `Cross-Origin-Resource-Policy: cross-origin` (observed), so it loads |
| Google Fonts files: `fonts.gstatic.com/s/...` | Requested by the CSS above | Font loads | It sends both `Access-Control-Allow-Origin: *` and CORP `cross-origin` (observed), so it passes whether the request is `cors` or `no-cors` |
| Vercel Web Analytics: `/_vercel/insights/script.js` | Same origin, injected in `src/index.html` | Same-origin | None. It also sends CORP `cross-origin` (observed) |
| `public/stuart.html`, `public/stuart-privacy.html` | Standalone static pages | Load nothing from another host; inline style only | None |
| PDFs and the PPTX in `public/`, opened with `target="_blank"` | Same origin | Navigation | None |
| `target="_blank"` links to GitHub, LinkedIn, Queen's, Tyler Technologies, and the order-book repo's docs | Cross-origin navigations, all with `rel="noopener noreferrer"` except the `mailto:` link | New top-level browsing context | COOP `same-origin` puts a cross-origin popup in a new browsing context group and cuts the opener reference [mdn-coop]. `noopener` already does that, and nothing on the site reads `window.opener` |
| `<iframe>`, `<embed>`, `<object>`, `<video>`, `<audio>`, `new Worker` | None in `src/` or `public/*.html` | — | — |
| `localStorage` and `sessionStorage` (achievements, the blackjack bank) | Per origin | — | Unaffected by isolation. It does matter for a separate subdomain ([§5](#5-alternatives)) |

The only thing found that would break is the **Vercel Toolbar on preview deployments**. Its documented CSP shows it loading scripts, images, frames, and fonts from `vercel.live` [vercel-toolbar-csp]. `https://vercel.live/_next-live/feedback/feedback.js` sends `Access-Control-Allow-Origin: *` but no `Cross-Origin-Resource-Policy` (observed). Loaded as a plain `no-cors` script, it would be blocked under `require-corp` [mdn-coep]. How the toolbar inserts its script is not stated, so this needs checking on a preview. Production deployments are unaffected.

## 2. Setting the headers on Vercel for this deploy

**What Vercel deploys.** Vercel's Angular framework preset detects `@angular/cli` and runs `ng build`. If `dist/` holds a single directory with a `browser/` folder inside it, that folder becomes the output directory. The default routes are `{ handle: 'filesystem' }`, then `{ src: '/(.*)', dest: '/index.html' }` [vercel-preset]. The site has no `vercel.json`, and the Vercel docs sitemap has no Angular-specific page [vercel-frameworks]. The live responses fit the preset:

- `GET /order-book` and `GET /order-book/` both return 200 with `Content-Disposition: inline; filename="order-book"`, `Cache-Control: public, max-age=0, must-revalidate`, `Content-Encoding: br`, and `X-Vercel-Cache: HIT` on repeat (observed).
- `GET /nonexistent-xyz` returns 200 with `filename="index.html"`, which is the SPA fallback, not an Angular SSR render (observed).
- `dist/thomas-boyle-website/prerendered-routes.json` lists `/`, `/blackjack`, `/capstone`, `/get-to-know-me`, and `/order-book`. Each is written as `browser/<route>/index.html`.

So the `CommonEngine` Express server in `server.ts` is built but never deployed. **Every HTML response is either a prerendered file or `index.html` served as the fallback.** No SSR response needs separate treatment. If the site later moved to real SSR on Vercel Functions, the same `vercel.json` headers would still apply, because Vercel documents them as covering "static files, Vercel functions, and a wildcard that matches all routes" [vercel-json-headers].

**Angular's own header mechanism doesn't help here.** `ServerRoute` has `headers` and `status` [ng-ssr], and `ServerRoutePrerender` keeps `headers` ("Optional additional headers to include in the response for this route") but drops `status` [ng-prerender-route]. The pages read don't say what a static host does with those headers. A CDN that serves files from `browser/` never reads Angular's route config, so on this deploy the headers would have no effect. (That last point is an inference, not documented.) The site also has no `app.routes.server.ts` and still uses `CommonEngine`.

**The `vercel.json` shape.** `headers` is an array of `{ source, headers: [{ key, value }], has?, missing? }`. `source` is "a pattern that matches each incoming pathname (excluding querystring)", and the documented examples use `/(.*)` and `/:path*` [vercel-json-headers]. `source` is the request path, not the file name [vercel-cache-control]. Each header entry counts against "Routes created per Deployment", which is 2048 on Hobby [vercel-limits]. Vercel won't let you modify the `x-matched-path`, `server`, and `content-length` response headers [vercel-cache-control]. For a route-only setup, cover both `/order-book` and `/order-book/`, since both serve the page (observed). A sitewide `/(.*)` covers both automatically.

**Worker scripts need the header too.** An HTTP(S) dedicated worker takes its policy container, including its embedder policy, from its own response [html-worker-policy]. If the owner document's COEP is compatible with isolation and the worker's isn't, the worker load fails: the HTML spec sets "response to a network error" [html-worker-coep] [html-worker-run]. Emscripten pthreads run on Web Workers [emscripten-pthreads], so the engine's `.js` (and any separate worker script) must be served with COEP as well. A per-route header on the HTML page alone is not enough. Worker scripts must also be same-origin (or `blob:`/`data:`) [mdn-worker], so the engine has to be served from the site's own origin, not from GitHub.

## 3. Isolation is per document: what client-side routing does

**The rule.** A browsing context group has a cross-origin isolation mode that "is initially `none`" [html-coi-mode]. The mode is set to `logical` or `concrete` only when a navigation's response has COOP `same-origin` together with a COEP compatible with isolation. That combination is the spec's `same-origin-plus-COEP` [html-coop-coep], and it takes effect when a new browsing context group is created for that navigation [html-obtain-bc]. A `Window`'s cross-origin isolated capability is true only if "realm's agent cluster's cross-origin-isolation mode is `concrete`" and the document is allowed to use the `cross-origin-isolated` permissions-policy feature [html-window-coi]. `self.crossOriginIsolated` reports that capability [html-dom-coi]. MDN's header pages give the same practical recipe: COOP `same-origin` plus COEP `require-corp` or `credentialless`, then check `crossOriginIsolated` [mdn-coep] [mdn-coop]. Unless the document is isolated, `postMessage()` throws for a `SharedArrayBuffer` and "the constructor on the global object is hidden" [mdn-sab].

**What that means for an Angular SPA.** The router swaps components inside the document that was loaded first, and the headers that counted were on that first response. So:

- **A visitor who loads `/order-book` directly** (typed URL, bookmark, external link, reload) gets the route's headers on the document and is isolated.
- **A visitor who lands on `/` and clicks through** to `/order-book` stays in the `/` document. With a route-only header, `crossOriginIsolated` is `false`, and the threaded engine can't start. The site has three such entry points: `routerLink="/order-book"` in `home-page.component.html`, `routerLink="/order-book"` in `navigation-bar.component.html` (with `refreshIfCurrentRoute`), and `[routerLink]="target.path"` in `sector-status.component.ts`, whose targets include `/order-book` (`achievement-definitions.ts`).
- **The reverse holds too.** A visitor who enters at `/order-book` (isolated) and clicks through to `/capstone` keeps the isolated document. So every page's resources must already be COEP-compatible, even with a route-only header. They are today ([§1](#1-what-the-site-loads-today)).

**Ways to force a real navigation into `/order-book`, if only that route is isolated:**

1. **Plain `href` instead of `routerLink`** on the three links. Angular's router only intercepts clicks on `RouterLink` elements, so a plain `<a href="/order-book">` makes the browser load a new document. (That is an inference from how `RouterLink` works, not a quoted doc.) It is fragile, because any new `routerLink` or `router.navigate` into the route silently loses isolation.
2. **A `canActivate` guard on the `order-book` route.** In the browser, when `!crossOriginIsolated`, it calls `location.assign('/order-book')` and returns `false`. Guards may return `boolean`, `UrlTree`/`RedirectCommand`, or a Promise/Observable, and `false` "blocks" the navigation [ng-guards]. The guide's warning "Do **not** return `false` and then programmatically `navigate` the user" is about router navigation, not a full page load, but it is worth noting. The guard must return `true` during prerender, which runs on the server, or prerendering of `/order-book` will fail. It also needs a loop breaker (for example a `sessionStorage` flag). Without one, a browser that ignores the headers, or a deploy that lost them, reloads forever. One place catches every entry path.
3. **A reload inside the page component** when it finds itself not isolated. This is the same idea as option 2, placed later. It renders the page once without isolation first, so it is worse than the guard.
4. **Isolate every document** (COOP/COEP on `/(.*)`). Then the first document a visitor loads is already isolated, and routing doesn't matter.

**Costs of a forced navigation.** It is a full reload. The prerendered `/order-book` HTML is 248,217 bytes before Brotli (observed `Content-Length`), and the reload discards in-memory Angular state. Achievements and the blackjack bank live in `localStorage`/`sessionStorage`, so they survive a same-origin reload. Whether `sessionStorage` survives a browsing-context-group switch is not stated in the spec text read, so check it on a preview before relying on it.

## 4. COEP `require-corp` vs `credentialless`

**What each does** [mdn-coep]:

- `require-corp`: a document "can only load resources requested in `no-cors` mode from the same origin, or resources that have explicitly set the `Cross-Origin-Resource-Policy` header to a value that allows it to be embedded."
- `credentialless`: `no-cors` cross-origin resources load without CORP, but "cookies are omitted in the request, and ignored in the response."
- Under both, `cors`-mode requests "won't be blocked by COEP … but must still be permitted by CORS."
- Setting the header twice, or with several tokens, is equivalent to `unsafe-none` [mdn-coep]. A duplicated `vercel.json` rule would silently turn isolation off.

**Iframes don't get easier under either value.** If the parent's policy is compatible with isolation, a child navigable's response must carry a compatible COEP too, or it is blocked [html-coep-checks]. web.dev says COEP must be on "all documents including those that are embedded with iframes and worker scripts" [webdev-coop-coep]. A cross-origin iframe also needs `allow="cross-origin-isolated"` to be isolated itself [webdev-coop-coep]. `<iframe credentialless>` (a different feature) exists only in Chrome 110+ and is marked experimental [bcd-iframe].

**Browser support** (MDN browser-compat-data, read 2026-11-09):

| Feature | Chrome / Edge | Chrome Android | Firefox | Firefox Android | Safari | Safari iOS | Source |
|---|---|---|---|---|---|---|---|
| COOP and COEP headers | 83 | 83 | 79 | 79 | 15.2 | 15.2 | [bcd-coep] [bcd-coop] |
| COEP `require-corp` (base header) | 83 | 83 | 79 | 79 | 15.2 | 15.2 | [bcd-coep] |
| COEP `credentialless` | 96 | 96 | 119 | **No** | **No** | **No** | [bcd-coep] |
| `crossOriginIsolated` | 87 | 87 | 72 | 72 | 15.2 | 15.2 | [bcd-coi] |
| `SharedArrayBuffer` | 68 | 89 | 79 | 79 | 15.2 | 15.2 | [bcd-sab] |
| `Document-Isolation-Policy` | 137 | 146 | No | No | No | No | [bcd-dip] |
| `CORP` header | 73 (Edge 79) | 73 | 74 | 74 | 12 | 12 | [bcd-corp] |

("Mirror" entries in browser-compat-data are shown as the desktop version they mirror.) web.dev still says of credentialless that "other browsers don't support it yet" beyond Chrome and Edge [webdev-coop-coep]. The compat data is newer and adds Firefox 119.

**What each value would break on this site:**

| Resource | `require-corp` | `credentialless` |
|---|---|---|
| Google Fonts CSS and font files | Loads (CORP `cross-origin`, ACAO `*`, observed) | Loads, minus cookies, which fonts don't need |
| Same-origin images, PDFs, bundles, analytics | Loads | Loads |
| Cross-origin images | None on the site | — |
| Iframes | None on the site; any future one needs its own COEP | Same rule |
| `target="_blank"` links | Unaffected by COEP. COOP `same-origin` puts cross-origin popups in a new group, which `noopener` already does [mdn-coop] | Same |
| Vercel Toolbar on previews | Probably blocked (no CORP on `vercel.live` script, observed) | Probably loads |
| Safari, iOS, Firefox Android visitors | Isolated | **Not isolated**: the value isn't supported [bcd-coep], so these browsers never see a COEP compatible with isolation. (MDN states only that a duplicated or multi-token header equals `unsafe-none` [mdn-coep]; treating an unknown value the same way is an inference) |

`credentialless` buys nothing on this site, because every cross-origin resource already sends CORP. It also loses the browsers listed above. **Use `require-corp`.**

## 5. Alternatives

**A service-worker shim (`coi-serviceworker`).** It is a service worker that adds COOP/COEP to responses, for hosts where you "can't set server headers" such as GitHub Pages. It "must be in a separate file", "can't be loaded from a CDN", and needs HTTPS or localhost. The first load triggers a reload [coi-sw]. Its options include `coepCredentialless`, `coepDegrade` (fall back to `require-corp` if credentialless fails), `quiet`, `shouldRegister`, `shouldDeregister`, and `doReload` [coi-sw]. It is MIT licensed [coi-sw]. The README doesn't discuss browser-specific failures. Vercel can set real headers, so the shim would only add a site-wide service worker, a forced reload on first visit, and its own update lifecycle, and it would solve nothing the headers don't. **Not needed.**

**`Document-Isolation-Policy` (Chrome only).** This header isolates one document. Its values are `isolate-and-require-corp` and `isolate-and-credentialless`. Documents using it "can open cross-origin popups and communicate with them" and "can embed any iframe normally" [chrome-dip]. It shipped in Chrome 137 on desktop and in Chrome 146 on Android [chrome-dip] [bcd-dip]. Firefox and Safari don't support it [bcd-dip]. It could isolate a component-level iframe or a non-COOP page in Chrome, but Firefox and Safari still need COOP/COEP. So it can be an extra on top of the real headers, never a replacement for them.

**A separate subdomain or Vercel project** (for example `demo.thomasboyle.dev`):

- On Hobby, a team can have 200 projects, and 25 projects can connect to one Git repository [vercel-limits]. A second project is allowed.
- It is a different origin, so `localStorage` and `sessionStorage` are separate. The site's achievements (`AchievementService`, unlocked from the order-book page) wouldn't carry across without extra plumbing.
- **Linked to as a top-level page**, it is isolated in its own right. But the demo leaves the site's chrome, needs its own deploy and pin, and is a second thing to keep in step with the case-study page.
- **Embedded in an iframe on a non-isolated `/order-book`**, it can't be isolated in standard HTML. A child navigable is created in its top-level browsing context's group [html-child-navigable], and that group's mode stays `"none"` without COOP+COEP on the top-level document [html-coi-mode] [html-window-coi]. Only Chrome's `Document-Isolation-Policy` gets around this.
- **Embedded in an isolated parent**, it needs its own COEP, CORP or CORS for its resources, and `allow="cross-origin-isolated"` [webdev-coop-coep] [html-coep-checks]. At that point the parent is already isolated, and the engine could run there directly.

**A popup window.** `window.open` of an isolated page creates a new browsing context group when the COOP values don't match [mdn-coop], so the popup can be isolated even if the opener isn't. It works, but the engine would run in a separate window beside the page, which is a poor fit for a demo embedded in the case study. Mentioned for completeness.

## 6. Free-plan limits that matter for a `.wasm`

| Topic | What applies | Source |
|---|---|---|
| Size of what you deploy | "When using the CLI to deploy, the maximum size of the source files that can be uploaded is limited to 100 MB for Hobby." No limit is stated for the output of Git-triggered builds. Build containers have 32 GB disk | [vercel-limits] |
| Bandwidth | Hobby includes "First 100 GB" of Fast Data Transfer a month and 10 GB of Fast Origin Transfer | [vercel-fair-use] |
| Requests | 1,000,000 function invocations a month on Hobby. Irrelevant, since everything is static | [vercel-fair-use] |
| Use | "Hobby teams are restricted to non-commercial personal use only" | [vercel-fair-use] |
| Compression | Brotli or gzip, chosen from the request's `Accept-Encoding`. `wasm` is on the "Automatically compressed MIME types" list, along with `javascript` | [vercel-compression] |
| Caching | Default `cache-control: public, max-age=0, must-revalidate`, "which instructs both the CDN and the browser not to cache". The recommendation for content-hashed assets is `max-age=31536000, immutable`. Static files are cached on the CDN "for the lifetime of the deployment after the first request". Today even the site's hashed `main-*.js` gets `max-age=0` (observed) | [vercel-cache-control] [vercel-cdn-cache] |
| Cache size limit | "Max cacheable response size: 10MB" is stated for Function responses. For static files, the pages say only that they are cached automatically | [vercel-cdn-cache] |
| MIME type | `WebAssembly.instantiateStreaming` rejects with a `TypeError` if the response isn't `application/wasm` [mdn-instantiate-streaming]. Vercel's compression list recognising `wasm` suggests it maps `.wasm` correctly, but the content type for `.wasm` is not stated anywhere read. Setting `Content-Type: application/wasm` in `vercel.json` for the engine path costs one header rule | [vercel-compression] [vercel-json-headers] |
| Routes | Every `vercel.json` header entry counts toward 2048 routes per deployment | [vercel-limits] |

**Serving from GitHub releases instead won't work.** A GitHub release download answers `302`, then `206` with `Content-Type: application/octet-stream` and no `Access-Control-Allow-Origin` (observed on a public repo's release asset). It would fail CORS for `fetch`, fail the MIME check for streaming compilation, and can't be a worker script at all [mdn-worker]. Either the website's build copies the pinned release artifact into `public/`, or #120 picks some other way to get the bytes onto the site's own origin.

## 7. Recommendation

**Isolate the whole site from `vercel.json`, with COOP `same-origin` and COEP `require-corp` on `/(.*)`.** Add a `Content-Type` and a long `Cache-Control` for the engine's versioned asset path. Keep a runtime `crossOriginIsolated` check on `/order-book` that switches to the fallback (fog on the map) when it is false.

```json
{
  "$schema": "https://openapi.vercel.sh/vercel.json",
  "headers": [
    {
      "source": "/(.*)",
      "headers": [
        { "key": "Cross-Origin-Opener-Policy", "value": "same-origin" },
        { "key": "Cross-Origin-Embedder-Policy", "value": "require-corp" }
      ]
    },
    {
      "source": "/order-book-engine/v2.2.0/(.*)\\.wasm",
      "headers": [
        { "key": "Content-Type", "value": "application/wasm" },
        { "key": "Cache-Control", "value": "public, max-age=31536000, immutable" }
      ]
    }
  ]
}
```

(The `/order-book-engine/v2.2.0/` path is a placeholder. #120 picks the real one. A versioned path is what makes `immutable` safe.)

Why this option:

- **It removes the routing problem instead of patching it.** Every document is isolated from its first byte, so the three `routerLink`s, any future `router.navigate`, and the `**` redirect need no change, and the reload loop can't happen ([§3](#3-isolation-is-per-document-what-client-side-routing-does)).
- **It costs the current site nothing.** Every cross-origin subresource is Google Fonts, which already sends CORP and CORS. Everything else is same-origin. There are no iframes and no `window.opener` use ([§1](#1-what-the-site-loads-today)).
- **It covers the worker scripts automatically.** The engine's `.js` gets COEP like everything else, as the worker rules require [html-worker-coep].
- **`require-corp` works in Chrome, Firefox, and Safari** on desktop and mobile. `credentialless` would lose Safari, iOS, and Firefox Android and gain nothing here ([§4](#4-coep-require-corp-vs-credentialless)).
- **Vercel's free plan handles it with one static file**, with no functions, middleware, or service worker ([§2](#2-setting-the-headers-on-vercel-for-this-deploy), [§6](#6-free-plan-limits-that-matter-for-a-wasm)).

What it costs, so #120 can weigh it:

- **A standing rule for the website.** Any future cross-origin embed (a YouTube video, a gist, a social widget, a cross-origin image) must send CORP or CORS, or it won't load anywhere on the site. Third-party iframes must also send COEP.
- **The Vercel Toolbar on preview deployments probably stops working** ([§1](#1-what-the-site-loads-today)). Production is unaffected.
- **A missing or duplicated header silently means "not isolated"** [mdn-coep]. The runtime check and the fallback are not optional.

**The fallback option, if the site-wide rule is unacceptable:** use the same two headers on `/order-book` and `/order-book/` only, plus the engine's asset path. Add a `canActivate` guard that turns a non-isolated router navigation into `location.assign('/order-book')`, with a once-per-session loop breaker, and returns `true` during prerender ([§3](#3-isolation-is-per-document-what-client-side-routing-does)). The visitor pays one full reload per entry by router link. Every page's resources must still be COEP-compatible, because a visitor who enters at `/order-book` keeps the isolated document while browsing the rest of the site.

**Not recommended:** the service-worker shim (headers are available), a separate subdomain or project (separate storage, a second deploy, and an iframe embed can't be isolated), or `Document-Isolation-Policy` as the main mechanism (Chrome only).

**To check on a preview deployment before relying on any of this:** that the response headers appear on `/`, `/order-book`, `/order-book/`, an unknown path (the fallback), and the engine's `.js` and `.wasm`; that `crossOriginIsolated` is `true` in Chrome, Firefox, and Safari after both a direct load and a router navigation; that `.wasm` comes back as `application/wasm` with `Content-Encoding: br`; that Google Fonts and the analytics beacon still load; and what the Vercel Toolbar does.

## Sources

- [vercel-preset] Vercel, `packages/frameworks/src/frameworks.ts`, the `Angular` entry (`getOutputDirName`, `defaultRoutes`): https://github.com/vercel/vercel/blob/main/packages/frameworks/src/frameworks.ts
- [vercel-frameworks] Vercel docs sitemap (no Angular page under `/docs/frameworks`): https://vercel.com/docs/sitemap
- [vercel-json-headers] Vercel docs, "Static Configuration with vercel.json", `headers`: https://vercel.com/docs/project-configuration/vercel-json#headers
- [vercel-cache-control] Vercel docs, "Cache-Control headers" (default value, recommended settings, custom response headers, reserved headers): https://vercel.com/docs/caching/cache-control-headers
- [vercel-cdn-cache] Vercel docs, "Vercel CDN Cache" (static files caching, cacheable response criteria, limits): https://vercel.com/docs/caching/cdn-cache
- [vercel-compression] Vercel docs, "Vercel CDN Compression": https://vercel.com/docs/how-vercel-cdn-works/compression
- [vercel-limits] Vercel docs, "Limits" (general limits, routes per deployment, static file uploads): https://vercel.com/docs/limits
- [vercel-fair-use] Vercel docs, "Fair Use Guidelines" (typical monthly usage, commercial usage): https://vercel.com/docs/limits/fair-use-guidelines
- [vercel-toolbar-csp] Vercel docs, "Managing the toolbar", "Using a Content Security Policy": https://vercel.com/docs/vercel-toolbar/managing-toolbar
- [ng-ssr] Angular, "Server and hybrid rendering", "Setting headers and status codes": https://angular.dev/guide/ssr
- [ng-prerender-route] Angular API, `ServerRoutePrerender`: https://angular.dev/api/ssr/ServerRoutePrerender
- [ng-guards] Angular, "Control route access with guards": https://angular.dev/guide/routing/route-guards
- [mdn-coep] MDN, `Cross-Origin-Embedder-Policy`: https://developer.mozilla.org/en-US/docs/Web/HTTP/Reference/Headers/Cross-Origin-Embedder-Policy
- [mdn-coop] MDN, `Cross-Origin-Opener-Policy`: https://developer.mozilla.org/en-US/docs/Web/HTTP/Reference/Headers/Cross-Origin-Opener-Policy
- [mdn-sab] MDN, `SharedArrayBuffer`, "Security requirements": https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/SharedArrayBuffer
- [mdn-worker] MDN, `Worker()` constructor: https://developer.mozilla.org/en-US/docs/Web/API/Worker/Worker
- [mdn-instantiate-streaming] MDN, `WebAssembly.instantiateStreaming()`: https://developer.mozilla.org/en-US/docs/WebAssembly/Reference/JavaScript_interface/instantiateStreaming_static
- [html-coi-mode] HTML Standard, browsing context group's cross-origin isolation mode: https://html.spec.whatwg.org/multipage/document-sequences.html#cross-origin-isolation-mode
- [html-child-navigable] HTML Standard, "create a new child navigable" (the child uses the top-level browsing context's group): https://html.spec.whatwg.org/multipage/document-sequences.html#create-a-new-child-navigable
- [html-coop-coep] HTML Standard, COOP `same-origin-plus-COEP`: https://html.spec.whatwg.org/multipage/browsers.html#coop-same-origin-plus-coep
- [html-obtain-bc] HTML Standard, "obtain a browsing context to use for a navigation response": https://html.spec.whatwg.org/multipage/browsers.html#obtain-browsing-context-navigation
- [html-coep-checks] HTML Standard, embedder policy checks (navigation response and worker): https://html.spec.whatwg.org/multipage/browsers.html#embedder-policy-checks
- [html-worker-coep] Same section, "check a global object's embedder policy": https://html.spec.whatwg.org/multipage/browsers.html#embedder-policy-checks
- [html-worker-policy] HTML Standard, "initialize a worker global scope's policy container": https://html.spec.whatwg.org/multipage/browsers.html#initialize-worker-policy-container
- [html-worker-run] HTML Standard, "run a worker": https://html.spec.whatwg.org/multipage/workers.html#run-a-worker
- [html-window-coi] HTML Standard, script settings for `Window` objects, "The cross-origin isolated capability": https://html.spec.whatwg.org/multipage/nav-history-apis.html#script-settings-for-window-objects
- [html-dom-coi] HTML Standard, `self.crossOriginIsolated`: https://html.spec.whatwg.org/multipage/webappapis.html#dom-crossoriginisolated
- [webdev-coop-coep] web.dev, "Making your website 'cross-origin isolated' using COOP and COEP": https://web.dev/articles/coop-coep
- [chrome-dip] Chrome for Developers, "Document-Isolation-Policy": https://developer.chrome.com/blog/document-isolation-policy
- [coi-sw] `gzuidhof/coi-serviceworker` README: https://github.com/gzuidhof/coi-serviceworker
- [emscripten-pthreads] Emscripten, "Pthreads support": https://emscripten.org/docs/porting/pthreads.html
- [bcd-coep] MDN browser-compat-data, `http/headers/Cross-Origin-Embedder-Policy.json`: https://github.com/mdn/browser-compat-data/blob/main/http/headers/Cross-Origin-Embedder-Policy.json
- [bcd-coop] MDN browser-compat-data, `http/headers/Cross-Origin-Opener-Policy.json`: https://github.com/mdn/browser-compat-data/blob/main/http/headers/Cross-Origin-Opener-Policy.json
- [bcd-corp] MDN browser-compat-data, `http/headers/Cross-Origin-Resource-Policy.json`: https://github.com/mdn/browser-compat-data/blob/main/http/headers/Cross-Origin-Resource-Policy.json
- [bcd-dip] MDN browser-compat-data, `http/headers/Document-Isolation-Policy.json`: https://github.com/mdn/browser-compat-data/blob/main/http/headers/Document-Isolation-Policy.json
- [bcd-coi] MDN browser-compat-data, `api/_globals/crossOriginIsolated.json`: https://github.com/mdn/browser-compat-data/blob/main/api/_globals/crossOriginIsolated.json
- [bcd-sab] MDN browser-compat-data, `javascript/builtins/SharedArrayBuffer.json`: https://github.com/mdn/browser-compat-data/blob/main/javascript/builtins/SharedArrayBuffer.json
- [bcd-iframe] MDN browser-compat-data, `api/HTMLIFrameElement.json` (`credentialless`): https://github.com/mdn/browser-compat-data/blob/main/api/HTMLIFrameElement.json
- Observed: `curl` on 2026-11-09 against `https://thomasboyle.dev/{order-book,order-book/,,nonexistent-xyz,stuart.html,index.csr.html,main-LJHYOOU4.js,_vercel/insights/script.js}`, `https://fonts.googleapis.com/css2?family=VT323&display=swap`, `https://fonts.googleapis.com/icon?family=Material+Icons`, `https://fonts.gstatic.com/s/vt323/v18/pxiKyp0ihIEF2hsY.ttf`, `https://vercel.live/_next-live/feedback/feedback.js`, and a public GitHub release asset (`WebAssembly/binaryen` `version_123`).
