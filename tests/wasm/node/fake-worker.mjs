// A worker that speaks another protocol than the client's, for the test that the client refuses
// it: it answers every request with protocol 2.
self.onmessage = (event) => {
    self.postMessage({id: event.data.id, ok: true, protocol: 2, release: "v0.0.0"});
};
