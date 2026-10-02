/* Run in every page before its own scripts: the web APIs that are easier to
 * write in script than in C.  web/prelude.h is generated from this file by
 * web/test/embed.pl. */
(function(){
var W = window, P = performance, marks = [];
P.mark = function(n){ var e = {name:n, entryType:'mark', startTime:P.now(), duration:0}; marks.push(e); return e; };
P.measure = function(n){ var e = {name:n, entryType:'measure', startTime:0, duration:P.now()}; marks.push(e); return e; };
P.getEntriesByName = function(n){ return marks.filter(function(e){ return e.name === n; }); };
P.getEntriesByType = function(t){ return marks.filter(function(e){ return e.entryType === t; }); };
P.getEntries = function(){ return marks.slice(); };
P.clearMarks = P.clearMeasures = function(){ marks = []; };
P.timeOrigin = 0; P.timing = {navigationStart:0}; P.navigation = {type:0};
function enc(s){ return encodeURIComponent(s).replace(/%20/g, '+'); }
function dec(s){ try { return decodeURIComponent(s.replace(/\+/g, ' ')); } catch (e) { return s; } }
class URLSearchParams {
  constructor(init){ this._l = []; if (init == null) return;
    if (typeof init === 'object') {
      if (Array.isArray(init)) { for (var p of init) this._l.push([String(p[0]), String(p[1])]); }
      else if (init._l) { this._l = init._l.map(function(p){ return [p[0], p[1]]; }); }
      else { for (var k in init) this._l.push([k, String(init[k])]); }
      return; }
    var s = String(init); if (s[0] === '?') s = s.slice(1);
    for (var part of s.split('&')) { if (!part) continue; var i = part.indexOf('=');
      this._l.push(i < 0 ? [dec(part), ''] : [dec(part.slice(0, i)), dec(part.slice(i + 1))]); } }
  get(k){ for (var p of this._l) if (p[0] === k) return p[1]; return null; }
  getAll(k){ return this._l.filter(function(p){ return p[0] === k; }).map(function(p){ return p[1]; }); }
  has(k){ return this.get(k) !== null; }
  set(k, v){ var done = false; this._l = this._l.filter(function(p){ if (p[0] !== k) return true; if (done) return false; p[1] = String(v); done = true; return true; });
    if (!done) this._l.push([k, String(v)]); if (this._u) this._u._sync(); }
  append(k, v){ this._l.push([k, String(v)]); if (this._u) this._u._sync(); }
  delete(k){ this._l = this._l.filter(function(p){ return p[0] !== k; }); if (this._u) this._u._sync(); }
  forEach(f, t){ for (var p of this._l) f.call(t, p[1], p[0], this); }
  keys(){ return this._l.map(function(p){ return p[0]; })[Symbol.iterator](); }
  values(){ return this._l.map(function(p){ return p[1]; })[Symbol.iterator](); }
  entries(){ return this._l.map(function(p){ return [p[0], p[1]]; })[Symbol.iterator](); }
  [Symbol.iterator](){ return this.entries(); }
  get size(){ return this._l.length; }
  sort(){ this._l.sort(function(a, b){ return a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0; }); }
  toString(){ return this._l.map(function(p){ return enc(p[0]) + '=' + enc(p[1]); }).join('&'); }
}
function resolve(u, base){
  if (/^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(u)) return u;
  if (base == null) throw new TypeError('Invalid URL: ' + u);
  var b = new URL(String(base));
  if (u.slice(0, 2) === '//') return b.protocol + u;
  if (u[0] === '/') return b.origin + u;
  if (u[0] === '?') return b.origin + b.pathname + u;
  if (u[0] === '#') return b.origin + b.pathname + b.search + u;
  if (u === '') return b.origin + b.pathname + b.search;
  var dir = b.pathname.slice(0, b.pathname.lastIndexOf('/') + 1), segs = (dir + u).split('/'), out = [];
  for (var i = 0; i < segs.length; i++) { var s = segs[i];
    if (s === '..') { if (out.length > 1) out.pop(); if (i === segs.length - 1) out.push(''); }
    else if (s === '.') { if (i === segs.length - 1) out.push(''); }
    else out.push(s); }
  return b.origin + out.join('/');
}
var URL_RX = /^([a-zA-Z][a-zA-Z0-9+.-]*:)(?:\/\/(?:([^:@\/]*)(?::([^@\/]*))?@)?([^:\/?#]*)(?::(\d+))?)?([^?#]*)(\?[^#]*)?(#.*)?$/;
class URL {
  constructor(u, base){ this._set(resolve(String(u), base)); }
  _set(s){
    var m = URL_RX.exec(s);
    if (!m) throw new TypeError('Invalid URL: ' + s);
    this.protocol = m[1].toLowerCase(); this.username = m[2] || ''; this.password = m[3] || '';
    this.hostname = (m[4] || '').toLowerCase(); this.port = m[5] || '';
    this.pathname = m[6] || (m[4] != null ? '/' : '');
    this.search = m[7] && m[7] !== '?' ? m[7] : ''; this.hash = m[8] && m[8] !== '#' ? m[8] : '';
    this.searchParams = new URLSearchParams(this.search); this.searchParams._u = this;
  }
  _sync(){ var q = this.searchParams.toString(); this.search = q ? '?' + q : ''; }
  get host(){ return this.hostname + (this.port ? ':' + this.port : ''); }
  get origin(){ return this.protocol + '//' + this.host; }
  get href(){ return this.origin + this.pathname + this.search + this.hash; }
  set href(v){ this._set(String(v)); }
  toString(){ return this.href; }
  toJSON(){ return this.href; }
  static canParse(u, b){ try { new URL(u, b); return true; } catch (e) { return false; } }
  static createObjectURL(){ return 'blob:null'; }
  static revokeObjectURL(){}
}
W.URL = URL; W.URLSearchParams = URLSearchParams;

/* event targets that are not elements (signals, ports, `class X extends EventTarget`) */
class EventTarget {
  addEventListener(t, f, o){ if (!f) return; var l = this.__ls || (this.__ls = {});
    (l[t] || (l[t] = [])).push({f: f, once: !!(o && typeof o === 'object' && o.once)}); }
  removeEventListener(t, f){ var l = this.__ls && this.__ls[t]; if (l) this.__ls[t] = l.filter(function(x){ return x.f !== f; }); }
  dispatchEvent(ev){
    try { if (!ev.target) ev.target = this; ev.currentTarget = this; } catch (e) {}
    var h = this['on' + ev.type]; if (typeof h === 'function') h.call(this, ev);
    var l = this.__ls && this.__ls[ev.type];
    if (l) for (var x of l.slice()) {
      if (x.once) this.removeEventListener(ev.type, x.f);
      if (typeof x.f === 'function') x.f.call(this, ev); else if (x.f && x.f.handleEvent) x.f.handleEvent(ev);
    }
    return !ev.defaultPrevented;
  }
}
class AbortSignal extends EventTarget {
  constructor(){ super(); this.aborted = false; this.reason = undefined; this.onabort = null; }
  throwIfAborted(){ if (this.aborted) throw this.reason; }
  static abort(r){ var c = new AbortController(); c.abort(r); return c.signal; }
  static timeout(ms){ var c = new AbortController(); setTimeout(function(){ c.abort(new Error('TimeoutError')); }, ms); return c.signal; }
  static any(list){ var c = new AbortController();
    for (var s of list) { if (s.aborted) { c.abort(s.reason); break; } s.addEventListener('abort', function(){ c.abort(this.reason); }); }
    return c.signal; }
}
class AbortController {
  constructor(){ this.signal = new AbortSignal(); }
  abort(r){ var s = this.signal; if (s.aborted) return; s.aborted = true;
    s.reason = r === undefined ? new Error('AbortError') : r; s.dispatchEvent({type: 'abort', target: s}); }
}
/* MessageChannel: messages arrive on the next turn (React's scheduler uses it) */
class MessagePort extends EventTarget {
  constructor(){ super(); this.onmessage = null; this._other = null; }
  postMessage(d){ var o = this._other; setTimeout(function(){ o.dispatchEvent({type: 'message', data: d}); }, 0); }
  start(){} close(){}
}
class MessageChannel {
  constructor(){ this.port1 = new MessagePort(); this.port2 = new MessagePort(); this.port1._other = this.port2; this.port2._other = this.port1; }
}
W.EventTarget = EventTarget; W.AbortSignal = AbortSignal; W.AbortController = AbortController;
W.MessagePort = MessagePort; W.MessageChannel = MessageChannel;
var EP = Element.prototype;
if (!EP.getRootNode) EP.getRootNode = function(){ var n = this; while (n.parentNode) n = n.parentNode; return n; };
if (typeof crypto === 'undefined') W.crypto = {
  getRandomValues: function(a){ for (var i = 0; i < a.length; i++) a[i] = Math.floor(Math.random() * 256); return a; },
  randomUUID: function(){ return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, function(c){
    var r = Math.floor(Math.random() * 16); return (c === 'x' ? r : (r & 3) | 8).toString(16); }); },
  subtle: {}
};
var D = document;
if (!D.styleSheets) D.styleSheets = [];
if (!D.adoptedStyleSheets) D.adoptedStyleSheets = [];
if (!D.fonts) D.fonts = { ready: Promise.resolve(), status: 'loaded', check: function(){ return true; },
  load: function(){ return Promise.resolve([]); }, add: function(){}, delete: function(){}, forEach: function(){},
  addEventListener: function(){}, removeEventListener: function(){} };
if (typeof CSSStyleSheet === 'undefined') W.CSSStyleSheet = class CSSStyleSheet {
  constructor(){ this.cssRules = []; } replaceSync(t){ this.text = t; } replace(t){ this.text = t; return Promise.resolve(this); }
  insertRule(r, i){ this.cssRules.splice(i || 0, 0, {cssText: r}); return i || 0; } deleteRule(i){ this.cssRules.splice(i, 1); }
};
if (typeof queueMicrotask !== 'function') W.queueMicrotask = function(f){ Promise.resolve().then(f); };
if (!String.raw) String.raw = function(s){ var r = s.raw, out = ''; for (var i = 0; i < r.length; i++) { out += r[i]; if (i + 1 < arguments.length && i + 1 < r.length) out += arguments[i + 1]; } return out; };
})();
