/* Tells the live view (js/live.js) that its data comes from the Arena on this PC after the owner has connected (control.js), not from a file or a server of this site; and fetch() for that Arena. */
window.LIVE_LOCAL = {};
window.LIVE_DEFAULT = { home: '../' };
(function () {
  const A = (window.A = window.A || {});
  // fetch() for requests to the Arena on this PC (127.0.0.1): says so to browsers that ask for the target of such a request (Chrome / Edge: Local Network Access)
  let lanOk = true;
  A.lfetch = (url, init) => (lanOk ? fetch(url, Object.assign({ targetAddressSpace: 'loopback' }, init)).catch((e) => {
    if (e instanceof TypeError && /address ?space|enum/i.test(String(e.message))) { lanOk = false; return fetch(url, init); }          // (a browser that knows the option under other values)
    throw e;
  }) : fetch(url, init));
})();
