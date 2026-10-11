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
/* iterators over arrays and array-likes (for-of on arrays is native, but
 * code also calls a[Symbol.iterator]() / .values() / .entries() itself) */
var mkIt = function(a, kind){ var i = 0; var it = { next: function(){
    if (i >= a.length) return {value: undefined, done: true};
    var k = i++; return {value: kind === 1 ? k : kind === 2 ? [k, a[k]] : a[k], done: false}; } };
  it[Symbol.iterator] = function(){ return it; }; return it; };
var AP = Array.prototype;
if (!AP[Symbol.iterator]) AP[Symbol.iterator] = function(){ return mkIt(this, 0); };
if (!AP.values) AP.values = function(){ return mkIt(this, 0); };
if (!AP.keys) AP.keys = function(){ return mkIt(this, 1); };
if (!AP.entries) AP.entries = function(){ return mkIt(this, 2); };
/* typed arrays: the methods that take callbacks (the rest are native, script_typed.c) */
var TP = W.__TypedArrayProto;
if (TP) {
  var mk = function(t, n){ return new t.constructor(n); };
  TP.forEach = function(f, th){ for (var i = 0; i < this.length; i++) f.call(th, this[i], i, this); };
  TP.map = function(f, th){ var r = mk(this, this.length); for (var i = 0; i < this.length; i++) r[i] = f.call(th, this[i], i, this); return r; };
  TP.filter = function(f, th){ var a = []; for (var i = 0; i < this.length; i++) if (f.call(th, this[i], i, this)) a.push(this[i]); return new this.constructor(a); };
  TP.reduce = function(f, acc){ var i = 0; if (arguments.length < 2) acc = this[i++]; for (; i < this.length; i++) acc = f(acc, this[i], i, this); return acc; };
  TP.reduceRight = function(f, acc){ var i = this.length - 1; if (arguments.length < 2) acc = this[i--]; for (; i >= 0; i--) acc = f(acc, this[i], i, this); return acc; };
  TP.every = function(f, th){ for (var i = 0; i < this.length; i++) if (!f.call(th, this[i], i, this)) return false; return true; };
  TP.some = function(f, th){ for (var i = 0; i < this.length; i++) if (f.call(th, this[i], i, this)) return true; return false; };
  TP.find = function(f, th){ for (var i = 0; i < this.length; i++) if (f.call(th, this[i], i, this)) return this[i]; };
  TP.findIndex = function(f, th){ for (var i = 0; i < this.length; i++) if (f.call(th, this[i], i, this)) return i; return -1; };
  TP.findLast = function(f, th){ for (var i = this.length - 1; i >= 0; i--) if (f.call(th, this[i], i, this)) return this[i]; };
  TP.findLastIndex = function(f, th){ for (var i = this.length - 1; i >= 0; i--) if (f.call(th, this[i], i, this)) return i; return -1; };
  TP.at = function(i){ i = Math.trunc(i) || 0; if (i < 0) i += this.length; return this[i]; };
  TP.sort = function(f){ var a = []; for (var i = 0; i < this.length; i++) a.push(this[i]);
    a.sort(f || function(x, y){ return x - y; }); for (var j = 0; j < a.length; j++) this[j] = a[j]; return this; };
  TP.toString = function(){ return this.join(','); };
  TP.toLocaleString = TP.toString;
  TP.keys = function(){ return mkIt(this, 1); };
  TP.values = function(){ return mkIt(this, 0); };
  TP.entries = function(){ return mkIt(this, 2); };
  TP[Symbol.iterator] = TP.values;
}
if (typeof TextEncoder === 'undefined') W.TextEncoder = class TextEncoder {
  get encoding(){ return 'utf-8'; }
  encode(s){ return __utf8_encode(s === undefined ? '' : String(s)); }
  encodeInto(s, dst){ var b = __utf8_encode(String(s)), n = Math.min(b.length, dst.length); dst.set(b.subarray(0, n)); return {read: s.length, written: n}; }
};
if (typeof TextDecoder === 'undefined') W.TextDecoder = class TextDecoder {
  constructor(l){ this.encoding = (l || 'utf-8').toLowerCase(); }
  decode(b){ return b === undefined ? '' : __utf8_decode(b); }
};
if (typeof Reflect === 'undefined') W.Reflect = {
  apply: function(f, t, a){ return f.apply(t, a || []); },
  construct: function(C, a){ var A = a || []; switch (A.length) { case 0: return new C(); case 1: return new C(A[0]);
    case 2: return new C(A[0], A[1]); case 3: return new C(A[0], A[1], A[2]); default: return new C(A[0], A[1], A[2], A[3]); } },
  get: function(o, k){ return o[k]; },
  set: function(o, k, v){ o[k] = v; return true; },
  has: function(o, k){ return k in o; },
  ownKeys: function(o){ return Object.getOwnPropertyNames(o); },
  defineProperty: function(o, k, d){ try { Object.defineProperty(o, k, d); return true; } catch (e) { return false; } },
  deleteProperty: function(o, k){ return delete o[k]; },
  getPrototypeOf: function(o){ return Object.getPrototypeOf(o); },
  setPrototypeOf: function(o, p){ Object.setPrototypeOf(o, p); return true; },
  getOwnPropertyDescriptor: function(o, k){ return Object.getOwnPropertyDescriptor(o, k); },
  isExtensible: function(){ return true; }, preventExtensions: function(){ return true; }
};
if (typeof escape !== 'function') {
  W.escape = function(s){ s = String(s); var r = ''; for (var i = 0; i < s.length; i++) { var c = s.charCodeAt(i), ch = s.charAt(i);
    if (/[A-Za-z0-9@*_+\-.\/]/.test(ch)) r += ch; else r += '%' + (c < 16 ? '0' : '') + c.toString(16).toUpperCase(); } return r; };
  W.unescape = function(s){ return String(s).replace(/%([0-9A-Fa-f]{2})/g, function(m, h){ return String.fromCharCode(parseInt(h, 16)); }); };
}
var M = Math;
if (!M.log1p) M.log1p = function(x){ return M.log(1 + x); };
if (!M.expm1) M.expm1 = function(x){ return M.exp(x) - 1; };
if (!M.sinh) M.sinh = function(x){ return (M.exp(x) - M.exp(-x)) / 2; };
if (!M.cosh) M.cosh = function(x){ return (M.exp(x) + M.exp(-x)) / 2; };
if (!M.tanh) M.tanh = function(x){ if (x > 20) return 1; if (x < -20) return -1; var a = M.exp(2 * x); return (a - 1) / (a + 1); };
var O = Object, OP = Object.prototype;
if (!O.isExtensible) O.isExtensible = function(){ return true; };
if (!O.getOwnPropertySymbols) O.getOwnPropertySymbols = function(){ return []; };
if (!O.getOwnPropertyDescriptors) O.getOwnPropertyDescriptors = function(o){ var r = {}; O.getOwnPropertyNames(o).forEach(function(k){ r[k] = O.getOwnPropertyDescriptor(o, k); }); return r; };
if (!OP.isPrototypeOf) OP.isPrototypeOf = function(o){ while (o != null) { o = O.getPrototypeOf(o); if (o === this) return true; } return false; };
if (!OP.propertyIsEnumerable) OP.propertyIsEnumerable = function(k){ return O.prototype.hasOwnProperty.call(this, k); };
if (!OP.valueOf) OP.valueOf = function(){ return this; };
if (!OP.toLocaleString) OP.toLocaleString = function(){ return this.toString(); };
var NP = Number.prototype;
if (!NP.toPrecision) NP.toPrecision = function(p){ if (p === undefined) return String(this); var x = Number(this);
  if (x === 0) return x.toFixed(p - 1); var e = Math.floor(Math.log10(Math.abs(x))); return (e < -6 || e >= p) ? x.toExponential(p - 1) : x.toFixed(Math.max(0, p - 1 - e)); };
if (!NP.toExponential) NP.toExponential = function(d){ var x = Number(this); if (x === 0) return (d ? (0).toFixed(d) : '0') + 'e+0';
  var e = Math.floor(Math.log10(Math.abs(x))), m = x / Math.pow(10, e); if (d === undefined) d = Math.max(0, String(m).replace('-', '').length - 2);
  var ms = m.toFixed(d); if (Math.abs(parseFloat(ms)) >= 10) { e++; ms = (m / 10).toFixed(d); } return ms + 'e' + (e < 0 ? '-' : '+') + Math.abs(e); };
if (!NP.toLocaleString) NP.toLocaleString = function(){ var p = String(this).split('.'); p[0] = p[0].replace(/\B(?=(\d{3})+(?!\d))/g, ','); return p.join('.'); };
var SP = String.prototype;
if (!SP.toLocaleLowerCase) SP.toLocaleLowerCase = SP.toLowerCase;
if (!SP.toLocaleUpperCase) SP.toLocaleUpperCase = SP.toUpperCase;
if (!Array.prototype.copyWithin) Array.prototype.copyWithin = function(t, s, e){ var n = this.length, c = this.slice(s, e === undefined ? n : e);
  for (var i = 0; i < c.length && t + i < n; i++) this[t + i] = c[i]; return this; };
if (!Function.prototype.toString) Function.prototype.toString = function(){ return 'function ' + (this.name || '') + '() { [native code] }'; };
if (typeof WeakRef === 'undefined') W.WeakRef = class WeakRef { constructor(t){ this._t = t; } deref(){ return this._t; } };
if (typeof Headers === 'undefined') W.Headers = class Headers {
  constructor(i){ this._h = {}; if (i) { if (i._h) i = i._h; for (var k in i) this._h[k.toLowerCase()] = String(i[k]); } }
  get(k){ var v = this._h[String(k).toLowerCase()]; return v === undefined ? null : v; }
  set(k, v){ this._h[String(k).toLowerCase()] = String(v); } append(k, v){ var o = this.get(k); this.set(k, o === null ? v : o + ', ' + v); }
  has(k){ return this.get(k) !== null; } delete(k){ delete this._h[String(k).toLowerCase()]; }
  forEach(f, t){ for (var k in this._h) f.call(t, this._h[k], k, this); }
  entries(){ var a = []; for (var k in this._h) a.push([k, this._h[k]]); return a[Symbol.iterator](); }
  [Symbol.iterator](){ return this.entries(); }
};
if (typeof FormData === 'undefined') W.FormData = class FormData {
  constructor(){ this._l = []; } append(k, v){ this._l.push([k, String(v)]); } set(k, v){ this.delete(k); this.append(k, v); }
  get(k){ for (var p of this._l) if (p[0] === k) return p[1]; return null; } getAll(k){ return this._l.filter(function(p){ return p[0] === k; }).map(function(p){ return p[1]; }); }
  has(k){ return this.get(k) !== null; } delete(k){ this._l = this._l.filter(function(p){ return p[0] !== k; }); }
  entries(){ return this._l.slice()[Symbol.iterator](); } [Symbol.iterator](){ return this.entries(); }
};
if (typeof Blob === 'undefined') W.Blob = class Blob {
  constructor(parts, o){ this._s = (parts || []).map(function(p){ return p && p.byteLength !== undefined && typeof p !== 'string' ? __utf8_decode(p) : String(p); }).join('');
    this.type = (o && o.type) || ''; }
  get size(){ return this._s.length; }
  text(){ return Promise.resolve(this._s); }
  arrayBuffer(){ return Promise.resolve(__utf8_encode(this._s).buffer); }
  slice(a, b, t){ var r = new Blob([this._s.slice(a, b)]); r.type = t || ''; return r; }
};
if (W.crypto && crypto.getRandomValues) crypto.getRandomValues = function(a){ for (var i = 0; i < a.length; i++) a[i] = Math.floor(Math.random() * 4294967296); return a; };
if (!Promise.withResolvers) Promise.withResolvers = function(){ var res, rej; var pr = new Promise(function(a, b){ res = a; rej = b; }); return {promise: pr, resolve: res, reject: rej}; };
/* more DOM: element insertion helpers, popovers, tree walkers, ranges, events, parsing */
var EPR = Element.prototype;
/* shadow roots are their elements here: each has its own adopted sheets */
if (!('adoptedStyleSheets' in EPR)) Object.defineProperty(EPR, 'adoptedStyleSheets', { configurable: true,
  get: function(){ return this.__adopted || (this.__adopted = []); }, set: function(v){ this.__adopted = v; } });
var toNode = function(x){ return typeof x === 'string' ? document.createTextNode(x) : x; };
if (!EPR.before) EPR.before = function(){ var p = this.parentNode; if (!p) return; for (var i = 0; i < arguments.length; i++) p.insertBefore(toNode(arguments[i]), this); };
if (!EPR.after) EPR.after = function(){ var p = this.parentNode; if (!p) return; var ref = this.nextSibling;
  for (var i = 0; i < arguments.length; i++) { var n = toNode(arguments[i]); if (ref) p.insertBefore(n, ref); else p.appendChild(n); } };
if (!EPR.showPopover) {
  EPR.showPopover = function(){ this.removeAttribute('hidden'); this.setAttribute('data-popover-open', ''); };
  EPR.hidePopover = function(){ this.removeAttribute('data-popover-open'); };
  EPR.togglePopover = function(f){ var open = this.hasAttribute('data-popover-open'); if (f === undefined ? !open : f) this.showPopover(); else this.hidePopover(); return !open; };
}
if (!EPR.checkVisibility) EPR.checkVisibility = function(){ return true; };
/* <template>.content: its children, moved into a fragment the first time */
Object.defineProperty(EPR, 'content', { get: function(){
  if (this.tagName !== 'TEMPLATE') return undefined;
  if (!this.__frag) { var f = document.createDocumentFragment(); while (this.firstChild) f.appendChild(this.firstChild); this.__frag = f; }
  return this.__frag; } });
/* shadow DOM: the shadow root is the element itself, so what a web
 * component puts in it is laid out and shown like normal content */
EPR.attachShadow = function(){ this.__shadow = this; return this; };
Object.defineProperty(EPR, 'shadowRoot', { get: function(){ return this.__shadow || null; } });
if (typeof W.ClipboardItem === 'undefined') W.ClipboardItem = class ClipboardItem {
  constructor(items){ this.items = items || {}; this.types = Object.keys(this.items); }
  getType(t){ return Promise.resolve(this.items[t]); }
  static supports(){ return true; }
};
var NF = { FILTER_ACCEPT: 1, FILTER_REJECT: 2, FILTER_SKIP: 3, SHOW_ALL: 0xFFFFFFFF, SHOW_ELEMENT: 1, SHOW_ATTRIBUTE: 2,
  SHOW_TEXT: 4, SHOW_COMMENT: 128, SHOW_DOCUMENT: 256, SHOW_DOCUMENT_FRAGMENT: 1024 };
if (typeof NodeFilter === 'undefined') W.NodeFilter = NF;
var showBit = function(n){ var t = n.nodeType; return t === 1 ? 1 : t === 3 ? 4 : t === 8 ? 128 : t === 9 ? 256 : t === 11 ? 1024 : 0; };
var TreeWalker = function(root, what, filter){ this.root = root; this.whatToShow = what === undefined ? NF.SHOW_ALL : what; this.filter = filter || null; this.currentNode = root; };
TreeWalker.prototype._ok = function(n){
  if (!(this.whatToShow & showBit(n))) return 3;
  if (!this.filter) return 1;
  var f = typeof this.filter === 'function' ? this.filter : this.filter.acceptNode;
  return f ? f.call(this.filter, n) : 1; };
TreeWalker.prototype._next = function(n, skipKids){
  if (!skipKids && n.firstChild) return n.firstChild;
  while (n && n !== this.root) { if (n.nextSibling) return n.nextSibling; n = n.parentNode; }
  return null; };
TreeWalker.prototype.nextNode = function(){
  var n = this.currentNode, skip = false;
  for (;;) { n = this._next(n, skip); if (!n) return null; var r = this._ok(n); skip = r === 2; if (r === 1) { this.currentNode = n; return n; } } };
TreeWalker.prototype.firstChild = function(){ var n = this.currentNode.firstChild; while (n && this._ok(n) !== 1) n = n.nextSibling; if (n) this.currentNode = n; return n || null; };
TreeWalker.prototype.nextSibling = function(){ var n = this.currentNode.nextSibling; while (n && this._ok(n) !== 1) n = n.nextSibling; if (n) this.currentNode = n; return n || null; };
TreeWalker.prototype.parentNode = function(){ var n = this.currentNode; while (n && n !== this.root) { n = n.parentNode; if (n && this._ok(n) === 1) { this.currentNode = n; return n; } } return null; };
TreeWalker.prototype.previousNode = function(){ return null; };
if (typeof W.TreeWalker === 'undefined') W.TreeWalker = TreeWalker;
var DOC = document;
if (!DOC.createTreeWalker) DOC.createTreeWalker = function(root, what, filter){ return new TreeWalker(root, what, filter); };
if (!DOC.createNodeIterator) DOC.createNodeIterator = function(root, what, filter){ var w = new TreeWalker(root, what, filter); w.previousNode = function(){ return null; }; return w; };
var DOMRect = function(x, y, w, h){ this.x = this.left = x || 0; this.y = this.top = y || 0; this.width = w || 0; this.height = h || 0;
  this.right = this.x + this.width; this.bottom = this.y + this.height; };
DOMRect.fromRect = function(r){ r = r || {}; return new DOMRect(r.x, r.y, r.width, r.height); };
if (typeof W.DOMRect === 'undefined') { W.DOMRect = DOMRect; W.DOMRectReadOnly = DOMRect; }
var Range = function(){ this.startContainer = this.endContainer = DOC; this.startOffset = this.endOffset = 0; this.collapsed = true; this.commonAncestorContainer = DOC; };
Range.prototype.setStart = function(n, o){ this.startContainer = n; this.startOffset = o; };
Range.prototype.setEnd = function(n, o){ this.endContainer = n; this.endOffset = o; this.collapsed = false; };
Range.prototype.setStartBefore = Range.prototype.setStartAfter = Range.prototype.setEndBefore = Range.prototype.setEndAfter = function(n){ this.startContainer = n; };
Range.prototype.selectNode = Range.prototype.selectNodeContents = function(n){ this.startContainer = this.endContainer = this.commonAncestorContainer = n; this.collapsed = false; };
Range.prototype.collapse = function(){ this.collapsed = true; };
Range.prototype.cloneRange = function(){ var r = new Range(); for (var k in this) if (this.hasOwnProperty(k)) r[k] = this[k]; return r; };
Range.prototype.getBoundingClientRect = function(){ var n = this.startContainer; return n && n.getBoundingClientRect ? n.getBoundingClientRect() : new DOMRect(); };
Range.prototype.getClientRects = function(){ return [this.getBoundingClientRect()]; };
Range.prototype.toString = function(){ var n = this.startContainer; return n ? (n.textContent || '') : ''; };
Range.prototype.createContextualFragment = function(html){ var t = DOC.createElement('template'); t.innerHTML = html;
  var f = DOC.createDocumentFragment(); var src = t.content || t; while (src.firstChild) f.appendChild(src.firstChild); return f; };
Range.prototype.deleteContents = Range.prototype.detach = function(){};
Range.prototype.insertNode = function(n){ var c = this.startContainer; if (c && c.appendChild) c.appendChild(n); };
if (typeof W.Range === 'undefined') W.Range = Range;
if (!DOC.createRange) DOC.createRange = function(){ return new Range(); };
if (!DOC.createEvent) DOC.createEvent = function(kind){
  var e = new CustomEvent('');
  e.initEvent = function(type, bubbles, cancelable){ this.type = type; this.bubbles = !!bubbles; this.cancelable = !!cancelable; };
  e.initCustomEvent = function(type, bubbles, cancelable, detail){ this.initEvent(type, bubbles, cancelable); this.detail = detail; };
  return e; };
if (!DOC.importNode) DOC.importNode = function(n, deep){ return n.cloneNode(!!deep); };
if (!DOC.adoptNode) DOC.adoptNode = function(n){ if (n.parentNode) n.parentNode.removeChild(n); return n; };
if (!DOC.elementFromPoint) DOC.elementFromPoint = function(){ return null; };
if (!DOC.elementsFromPoint) DOC.elementsFromPoint = function(){ return []; };
if (!DOC.startViewTransition) DOC.startViewTransition = function(cb){ var p = Promise.resolve().then(function(){ return cb && cb(); });
  return { finished: p, ready: Promise.resolve(), updateCallbackDone: p, skipTransition: function(){} }; };
if (typeof W.DOMParser === 'undefined') W.DOMParser = class DOMParser {
  parseFromString(s){ var html = DOC.createElement('html'); html.innerHTML = String(s);
    var body = html.querySelector('body') || html, head = html.querySelector('head') || DOC.createElement('head');
    return { documentElement: html, body: body, head: head, title: (html.querySelector('title') || {}).textContent || '',
      querySelector: function(q){ return html.querySelector(q); }, querySelectorAll: function(q){ return html.querySelectorAll(q); },
      getElementById: function(id){ return html.querySelector('#' + id); }, getElementsByTagName: function(t){ return html.querySelectorAll(t); },
      createElement: function(t){ return DOC.createElement(t); } }; }
};
if (typeof W.XMLSerializer === 'undefined') W.XMLSerializer = class XMLSerializer { serializeToString(n){ return n.outerHTML !== undefined ? n.outerHTML : String(n.textContent || ''); } };
if (typeof W.Option === 'undefined') W.Option = function(text, value, def, sel){ var o = DOC.createElement('option');
  if (text !== undefined) o.textContent = text; if (value !== undefined) o.setAttribute('value', value); if (sel) o.setAttribute('selected', ''); return o; };
if (typeof W.FinalizationRegistry === 'undefined') W.FinalizationRegistry = class FinalizationRegistry { register(){} unregister(){ return false; } };
if (typeof W.BroadcastChannel === 'undefined') W.BroadcastChannel = class BroadcastChannel { constructor(n){ this.name = n; this.onmessage = null; }
  postMessage(){} close(){} addEventListener(){} removeEventListener(){} };
if (typeof W.File === 'undefined' && typeof Blob !== 'undefined') W.File = class File extends Blob {
  constructor(parts, name, o){ super(parts, o); this.name = String(name); this.lastModified = Date.now(); } };
if (typeof W.FileReader === 'undefined') W.FileReader = class FileReader {
  constructor(){ this.result = null; this.readyState = 0; this.onload = this.onloadend = this.onerror = null; }
  _done(r){ var me = this; me.result = r; me.readyState = 2; setTimeout(function(){ var e = {type: 'load', target: me};
    if (me.onload) me.onload(e); if (me.onloadend) me.onloadend(e); }, 0); }
  readAsText(b){ this._done(b && b._s !== undefined ? b._s : String(b)); }
  readAsDataURL(b){ this._done('data:' + ((b && b.type) || 'application/octet-stream') + ';base64,' + btoa(b && b._s !== undefined ? b._s : '')); }
  readAsArrayBuffer(b){ this._done(__utf8_encode(b && b._s !== undefined ? b._s : '').buffer); }
  abort(){} addEventListener(t, f){ if (t === 'load') this.onload = f; else if (t === 'loadend') this.onloadend = f; else if (t === 'error') this.onerror = f; }
};
if (typeof W.Response === 'undefined') W.Response = class Response {
  constructor(body, init){ init = init || {}; this._b = body == null ? '' : (typeof body === 'string' ? body : body._s !== undefined ? body._s : String(body));
    this.status = init.status === undefined ? 200 : init.status; this.statusText = init.statusText || ''; this.ok = this.status >= 200 && this.status < 300;
    this.headers = new Headers(init.headers); this.url = ''; this.type = 'default'; this.redirected = false; this.bodyUsed = false; }
  text(){ return Promise.resolve(this._b); }
  json(){ var me = this; return Promise.resolve().then(function(){ return JSON.parse(me._b); }); }
  blob(){ return Promise.resolve(new Blob([this._b])); }
  arrayBuffer(){ return Promise.resolve(__utf8_encode(this._b).buffer); }
  clone(){ return new Response(this._b, {status: this.status, statusText: this.statusText, headers: this.headers}); }
  static json(d, init){ return new Response(JSON.stringify(d), init); }
  static error(){ return new Response('', {status: 0}); }
};
if (typeof W.Request === 'undefined') W.Request = class Request {
  constructor(input, init){ init = init || {}; this.url = typeof input === 'string' ? input : input.url; this.method = (init.method || 'GET').toUpperCase();
    this.headers = new Headers(init.headers); this.body = init.body === undefined ? null : init.body; this.credentials = init.credentials || 'same-origin';
    this.mode = init.mode || 'cors'; this.signal = init.signal || null; }
  clone(){ return new Request(this.url, this); }
};
if (typeof W.Intl === 'undefined') {
  var pad2 = function(n){ return (n < 10 ? '0' : '') + n; };
  var MON = ['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];
  var DTF = function(loc, o){ this.o = o || {}; };
  DTF.prototype.format = function(d){ d = d === undefined ? new Date() : (d instanceof Date ? d : new Date(d)); var o = this.o;
    if (o.hour !== undefined && o.year === undefined && o.month === undefined) return pad2(d.getHours()) + ':' + pad2(d.getMinutes());
    var s = MON[d.getMonth()] + ' ' + d.getDate() + ', ' + d.getFullYear();
    if (o.hour !== undefined) s += ', ' + pad2(d.getHours()) + ':' + pad2(d.getMinutes());
    return s; };
  DTF.prototype.formatToParts = function(d){ return [{type: 'literal', value: this.format(d)}]; };
  DTF.prototype.resolvedOptions = function(){ return {locale: 'en-US', timeZone: 'UTC', calendar: 'gregory', numberingSystem: 'latn'}; };
  var NFm = function(loc, o){ this.o = o || {}; };
  NFm.prototype.format = function(n){ var o = this.o, x = Number(n);
    if (o.maximumFractionDigits !== undefined) x = Number(x.toFixed(o.maximumFractionDigits));
    var s = x.toLocaleString(); if (o.style === 'percent') s = (x * 100).toFixed(0) + '%';
    if (o.style === 'currency') s = (o.currency === 'EUR' ? '€' : '$') + x.toFixed(2);
    return s; };
  NFm.prototype.formatToParts = function(n){ return [{type: 'integer', value: this.format(n)}]; };
  NFm.prototype.resolvedOptions = function(){ return {locale: 'en-US'}; };
  var RTF = function(){};
  RTF.prototype.format = function(v, unit){ v = Number(v); var u = String(unit).replace(/s$/, ''), a = Math.abs(v);
    var w = a + ' ' + u + (a === 1 ? '' : 's'); return v < 0 ? w + ' ago' : 'in ' + w; };
  var PR = function(){}; PR.prototype.select = function(n){ return n === 1 ? 'one' : 'other'; };
  var COL = function(){}; COL.prototype.compare = function(a, b){ a = String(a); b = String(b); return a < b ? -1 : a > b ? 1 : 0; };
  var LF = function(){}; LF.prototype.format = function(l){ l = Array.from(l); return l.length < 2 ? l.join('') : l.slice(0, -1).join(', ') + ' and ' + l[l.length - 1]; };
  W.Intl = { DateTimeFormat: DTF, NumberFormat: NFm, RelativeTimeFormat: RTF, PluralRules: PR, Collator: COL, ListFormat: LF,
    getCanonicalLocales: function(l){ return l ? [].concat(l) : []; },
    Segmenter: function(){ this.segment = function(s){ return Array.from(String(s)).map(function(c, i){ return {segment: c, index: i}; }); }; } };
  if (Date.prototype) Date.prototype.toLocaleDateString = function(){ return new DTF().format(this); };
}
if (typeof W.DOMException === 'undefined') {
  var DE_CODES = { IndexSizeError: 1, HierarchyRequestError: 3, WrongDocumentError: 4, InvalidCharacterError: 5,
    NoModificationAllowedError: 7, NotFoundError: 8, NotSupportedError: 9, InvalidStateError: 11, SyntaxError: 12,
    InvalidModificationError: 13, NamespaceError: 14, InvalidAccessError: 15, TypeMismatchError: 17, SecurityError: 18,
    NetworkError: 19, AbortError: 20, URLMismatchError: 21, QuotaExceededError: 22, TimeoutError: 23,
    InvalidNodeTypeError: 24, DataCloneError: 25 };
  W.DOMException = class DOMException extends Error {
    constructor(message, name) { super(message === undefined ? '' : String(message)); this.message = message === undefined ? '' : String(message); this.name = name || 'Error'; this.code = DE_CODES[this.name] || 0; }
  };
  for (var dk in DE_CODES) W.DOMException[dk.replace(/([a-z])([A-Z])/g, '$1_$2').toUpperCase().replace(/_ERROR$/, '_ERR')] = DE_CODES[dk];
}
(function(N){
  if (!N) return;
  var def = function(k, v){ if (N[k] === undefined) N[k] = v; };
  def('languages', [N.language || 'en-US', 'en']);
  def('hardwareConcurrency', 1); def('maxTouchPoints', 0); def('deviceMemory', 4);
  def('vendor', ''); def('product', 'Gecko'); def('appCodeName', 'Mozilla'); def('appVersion', '5.0 (Banana OS)');
  def('doNotTrack', null); def('webdriver', false); def('pdfViewerEnabled', false);
  def('plugins', []); def('mimeTypes', []);
  def('connection', { effectiveType: '4g', downlink: 10, rtt: 50, saveData: false, addEventListener: function(){}, removeEventListener: function(){} });
  def('sendBeacon', function(){ return true; });
  def('vibrate', function(){ return false; });
  def('javaEnabled', function(){ return false; });
  def('clipboard', { writeText: function(){ return Promise.resolve(); }, readText: function(){ return Promise.resolve(''); },
                     write: function(){ return Promise.resolve(); }, read: function(){ return Promise.resolve([]); } });
  def('permissions', { query: function(){ return Promise.resolve({ state: 'prompt', onchange: null, addEventListener: function(){} }); } });
  def('storage', { estimate: function(){ return Promise.resolve({ quota: 0, usage: 0 }); }, persist: function(){ return Promise.resolve(false); } });
})(W.navigator);
/* web views: the app's web_post(text) arrives as a "message" event */
W.__banana_deliver = function(data){
  var e; try { e = new MessageEvent('message', { data: data, origin: 'banana:' }); } catch (x) { e = new Event('message'); e.data = data; e.origin = 'banana:'; }
  if (typeof W.onmessage === 'function') { try { W.onmessage(e); } catch (x) {} }
  W.dispatchEvent(e);
};
/* <audio> / <video> / new Audio(): sound through the system's media streams
 * (__media_*: kernel/media.c). The element's state lives in el.__ms; a timer
 * polls the stream and fires the usual events. */
(function(){
  if (typeof __media_open !== 'function') { W.Audio = W.Audio || function(src){ var a = document.createElement('audio'); if (src !== undefined) a.setAttribute('src', src); return a; }; return; }
  var PLAY = 1, PAUSE = 2, SEEK = 3, SET = 4, CLOSE = 5;
  function isMedia(el){ var t = el && el.tagName; return t === 'AUDIO' || t === 'VIDEO'; }
  function st(el){
    return el.__ms || (el.__ms = { id: -1, url: '', vol: 1, muted: false, loop: false, paused: true, ended: false,
                                   pos: 0, dur: NaN, ready: 0, seq: 0, rate: 1, err: null, seeking: false, playing: false });
  }
  function srcOf(el){
    var s = el.getAttribute('src');
    if (!s) { var so = el.querySelector('source[src]'); if (so) s = so.getAttribute('src'); }
    if (!s) return '';
    try { return new URL(s, location.href).href; } catch (x) { return s; }
  }
  function fire(el, type){ try { el.dispatchEvent(new Event(type)); } catch (x) {} }
  var active = [], timer = 0;
  function track(el){ if (active.indexOf(el) < 0) active.push(el); if (!timer) timer = setInterval(poll, 250); }
  function pushSet(el){ var m = st(el); if (m.id >= 0) __media_cmd(m.id, SET, Math.round(m.vol * 100), m.muted ? 1 : 0, (m.loop || el.hasAttribute('loop')) ? 1 : 0); }
  function ensure(el){
    var m = st(el), u = srcOf(el);
    if (u && u !== m.url) {
      if (m.id >= 0) __media_cmd(m.id, CLOSE);
      m.url = u; m.id = __media_open(u); m.ready = 0; m.dur = NaN; m.pos = 0; m.ended = false; m.err = null;
      pushSet(el);
      fire(el, 'loadstart');
      track(el);
    }
    return m;
  }
  function poll(){
    for (var i = 0; i < active.length; i++) {
      var el = active[i], m = st(el);
      if (m.id < 0) continue;
      var s = __media_status(m.id);
      if (!s) continue;
      if (s.state === 2 && m.ready < 4) {
        m.ready = 4; m.dur = s.dur;
        fire(el, 'durationchange'); fire(el, 'loadedmetadata'); fire(el, 'loadeddata'); fire(el, 'canplay'); fire(el, 'canplaythrough');
        if (m.paused && el.hasAttribute('autoplay')) el.play();
      }
      if (s.state === 3 && !m.err) { m.err = { code: 4, message: s.error, MEDIA_ERR_SRC_NOT_SUPPORTED: 4 }; m.paused = true; fire(el, 'error'); }
      if (s.playing && !m.playing) { m.playing = true; fire(el, 'playing'); }
      if (!s.playing) m.playing = false;
      if (s.pos !== m.pos && !m.seeking) { m.pos = s.pos; fire(el, 'timeupdate'); }
      if (s.seq !== m.seq) { m.seq = s.seq; if (m.seeking) { m.seeking = false; m.pos = s.pos; fire(el, 'seeked'); fire(el, 'timeupdate'); } }
      if (s.ended && !m.ended && !m.paused) { m.ended = true; m.paused = true; m.pos = m.dur; fire(el, 'timeupdate'); fire(el, 'pause'); fire(el, 'ended'); }
    }
  }
  EPR.play = function(){
    if (!isMedia(this)) return Promise.resolve();
    var m = ensure(this);
    if (m.id < 0) return Promise.reject(new DOMException('The element has no supported sources.', 'NotSupportedError'));
    if (m.paused) { m.paused = false; m.ended = false; __media_cmd(m.id, PLAY); fire(this, 'play'); }
    return Promise.resolve();
  };
  EPR.pause = function(){
    if (!isMedia(this)) return;
    var m = st(this);
    if (m.paused) return;
    m.paused = true;
    if (m.id >= 0) __media_cmd(m.id, PAUSE);
    fire(this, 'pause');
  };
  EPR.load = function(){ if (!isMedia(this)) return; var m = st(this); m.url = ''; m.paused = true; ensure(this); };
  EPR.canPlayType = function(t){
    t = String(t || '').toLowerCase();
    return /audio\/(mpeg|mp3|mpeg3|x-mpeg|wav|wave|x-wav|flac|x-flac)/.test(t) ? 'probably' : '';
  };
  EPR.fastSeek = function(t){ this.currentTime = t; };
  function prop(name, get, set){
    Object.defineProperty(EPR, name, { configurable: true,
      get: function(){ return isMedia(this) ? get.call(this, st(this)) : this['__' + name]; },
      set: function(v){ if (isMedia(this) && set) set.call(this, st(this), v); else this['__' + name] = v; } });
  }
  prop('currentTime', function(m){ return m.pos; }, function(m, v){
    v = +v || 0; if (v < 0) v = 0;
    var mm = ensure(this); m.pos = v; m.seeking = true; m.ended = false;
    fire(this, 'seeking');
    if (mm.id >= 0) __media_cmd(mm.id, SEEK, Math.round(v * 1000));
  });
  prop('duration', function(m){ return m.dur; });
  prop('paused', function(m){ return m.paused; });
  prop('ended', function(m){ return m.ended; });
  prop('volume', function(m){ return m.vol; }, function(m, v){ m.vol = Math.max(0, Math.min(1, +v || 0)); pushSet(this); fire(this, 'volumechange'); });
  prop('muted', function(m){ return m.muted; }, function(m, v){ m.muted = !!v; pushSet(this); fire(this, 'volumechange'); });
  prop('loop', function(m){ return m.loop || this.hasAttribute('loop'); }, function(m, v){ m.loop = !!v; pushSet(this); });
  prop('autoplay', function(m){ return this.hasAttribute('autoplay'); }, function(m, v){ if (v) this.setAttribute('autoplay', ''); else this.removeAttribute('autoplay'); });
  prop('readyState', function(m){ return m.ready; });
  prop('networkState', function(m){ return m.id < 0 ? 0 : m.ready ? 1 : 2; });
  prop('error', function(m){ return m.err; });
  prop('seeking', function(m){ return m.seeking; });
  prop('playbackRate', function(m){ return m.rate; }, function(m, v){ m.rate = +v || 1; });
  prop('defaultPlaybackRate', function(m){ return 1; }, function(m, v){});
  prop('currentSrc', function(m){ return m.url; });
  prop('preload', function(m){ return this.getAttribute('preload') || 'auto'; }, function(m, v){ this.setAttribute('preload', v); });
  prop('buffered', function(m){ var d = m.ready ? m.dur : 0, n = m.ready ? 1 : 0;
    return { length: n, start: function(){ return 0; }, end: function(){ return d; } }; });
  prop('played', function(m){ return { length: 0, start: function(){ return 0; }, end: function(){ return 0; } }; });
  prop('seekable', function(m){ var d = m.ready ? m.dur : 0, n = m.ready ? 1 : 0;
    return { length: n, start: function(){ return 0; }, end: function(){ return d; } }; });
  /* src: an attribute; a new one loads when it plays (or now, with autoplay / preload) */
  W.Audio = function(src){ var a = document.createElement('audio'); if (src !== undefined) a.setAttribute('src', src); return a; };
  W.Audio.prototype = EPR;
  if (W.HTMLMediaElement) { W.HTMLMediaElement.HAVE_NOTHING = 0; W.HTMLMediaElement.HAVE_METADATA = 1; W.HTMLMediaElement.HAVE_ENOUGH_DATA = 4; }
  if (typeof W.HTMLAudioElement === 'undefined') W.HTMLAudioElement = W.Audio;
  if (typeof W.MediaMetadata === 'undefined') W.MediaMetadata = function(o){ o = o || {}; this.title = o.title || ''; this.artist = o.artist || ''; this.album = o.album || ''; this.artwork = o.artwork || []; };
  if (W.navigator && !W.navigator.mediaSession) W.navigator.mediaSession = { metadata: null, playbackState: 'none', setActionHandler: function(){}, setPositionState: function(){} };
})();
})();

/* PerformanceObserver.supportedEntryTypes: pages ask before observing */
try { if (typeof PerformanceObserver !== 'undefined' && !PerformanceObserver.supportedEntryTypes) PerformanceObserver.supportedEntryTypes = []; } catch (e) {}
