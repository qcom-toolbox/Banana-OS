/* SPDX-License-Identifier: GPL-3.0-or-later */
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
W.FormData = class FormData {
  constructor(form){ this._l = [];
    if (form && form.querySelectorAll) { var els = form.querySelectorAll('input,select,textarea');
      for (var i = 0; i < els.length; i++) { var e = els[i], n = e.getAttribute('name'); if (!n || e.hasAttribute('disabled')) continue;
        var t = (e.getAttribute('type') || '').toLowerCase();
        if (t === 'submit' || t === 'button' || t === 'reset' || t === 'image') continue;
        if ((t === 'checkbox' || t === 'radio') && !e.checked) continue;
        if (t === 'file') { var fl = e.files; if (fl && fl.length) for (var k = 0; k < fl.length; k++) this.append(n, fl[k]);
          else this.append(n, new File([], '', {type: 'application/octet-stream'})); continue; }
        this.append(n, (t === 'checkbox' || t === 'radio') ? (e.getAttribute('value') || 'on') : (e.value || '')); } } }
  append(k, v, fn){ k = String(k); if (v instanceof Blob) this._l.push([k, v, fn !== undefined ? String(fn) : (v.name !== undefined ? v.name : 'blob')]); else this._l.push([k, String(v)]); }
  set(k, v, fn){ this.delete(k); this.append(k, v, fn); }
  get(k){ for (var p of this._l) if (p[0] === k) return p[1]; return null; }
  getAll(k){ return this._l.filter(function(p){ return p[0] === k; }).map(function(p){ return p[1]; }); }
  has(k){ return this.get(k) !== null; } delete(k){ this._l = this._l.filter(function(p){ return p[0] !== k; }); }
  entries(){ return this._l.map(function(p){ return [p[0], p[1]]; })[Symbol.iterator](); }
  keys(){ return this._l.map(function(p){ return p[0]; })[Symbol.iterator](); }
  values(){ return this._l.map(function(p){ return p[1]; })[Symbol.iterator](); }
  forEach(f, t){ var me = this; this._l.forEach(function(p){ f.call(t, p[1], p[0], me); }); }
  [Symbol.iterator](){ return this.entries(); }
};
if (typeof Blob === 'undefined') W.Blob = class Blob {
  constructor(parts, o){ this._s = (parts || []).map(function(p){ return p && p._path !== undefined ? __utf8_decode(__readFile(p._path) || new Uint8Array(0)) :
      p && p._s !== undefined ? p._s : p && p.byteLength !== undefined && typeof p !== 'string' ? __utf8_decode(p) : String(p); }).join('');
    this.type = (o && o.type) || ''; }
  get size(){ return this._path !== undefined ? this._size : __utf8_encode(this._s).length; }
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
  readAsText(b){ this._done(__blobStr(b)); }
  readAsDataURL(b){ this._done('data:' + ((b && b.type) || 'application/octet-stream') + ';base64,' + btoa(__blobStr(b))); }
  readAsArrayBuffer(b){ this._done((b && b._path !== undefined ? (__readFile(b._path) || new Uint8Array(0)) : __utf8_encode(__blobStr(b))).buffer); }
  readAsBinaryString(b){ this._done(__blobStr(b)); }
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
/* <canvas>: getContext('2d') - CanvasRenderingContext2D drawn by web/canvas.c
 * through __cv(element, op, ...). The JS side keeps the readable state
 * (fillStyle, font...) and Path2D; pixels and paths live in C. */
(function(){
  if (typeof __cv !== 'function') return;
  var CAP = { butt: 0, round: 1, square: 2 }, JOIN = { miter: 0, round: 1, bevel: 2 };
  var COMP = { 'source-over': 0, 'destination-out': 1, 'lighter': 2, 'copy': 3, 'destination-over': 4,
               'source-atop': 5, 'destination-in': 6, 'source-in': 7 };
  var ALIGN = { start: 0, left: 0, center: 1, right: 2, end: 2 };
  var BASE = { alphabetic: 0, top: 1, hanging: 1, middle: 2, bottom: 3, ideographic: 3 };
  var HEX = []; for (var hx = 0; hx < 256; hx++) HEX.push((hx < 16 ? '0' : '') + hx.toString(16));
  function CanvasGradient(id){ this.__g = id; }
  CanvasGradient.prototype.addColorStop = function(o, c){ __cv(null, 24, this.__g, +o, String(c)); };
  function CanvasPattern(id){ this.__p = id; }
  CanvasPattern.prototype.setTransform = function(){};
  function ImageData(a, b, c){
    if (typeof a === 'number') { this.width = a; this.height = b; this.data = new Uint8ClampedArray(a * b * 4); }
    else { this.data = a; this.width = b; this.height = c || (a.length / 4 / b); }
    this.colorSpace = 'srgb';
  }
  function Path2D(p){
    this.__ops = [];
    if (p instanceof Path2D) this.__ops = p.__ops.slice();
    else if (typeof p === 'string') svgPath(this, p);
  }
  var PATH_OPS = ['moveTo', 'lineTo', 'closePath', 'rect', 'roundRect', 'arc', 'arcTo', 'quadraticCurveTo',
                  'bezierCurveTo', 'ellipse'];
  PATH_OPS.forEach(function(n){ Path2D.prototype[n] = function(){ this.__ops.push([n, Array.prototype.slice.call(arguments)]); }; });
  Path2D.prototype.addPath = function(p){ if (p && p.__ops) this.__ops = this.__ops.concat(p.__ops); };
  /* SVG path data ("M10 10 L20 20 Z"), absolute and relative commands */
  function svgPath(path, d){
    var t = String(d).match(/[a-zA-Z]|-?[0-9]*\.?[0-9]+(?:e[-+]?[0-9]+)?/g) || [], i = 0, cmd = '', x = 0, y = 0, sx = 0, sy = 0;
    function n(){ return +t[i++]; }
    while (i < t.length) {
      if (/[a-zA-Z]/.test(t[i])) cmd = t[i++];
      var rel = cmd === cmd.toLowerCase(), C = cmd.toUpperCase(), ox = rel ? x : 0, oy = rel ? y : 0;
      if (C === 'M') { x = ox + n(); y = oy + n(); sx = x; sy = y; path.moveTo(x, y); cmd = rel ? 'l' : 'L'; }
      else if (C === 'L') { x = ox + n(); y = oy + n(); path.lineTo(x, y); }
      else if (C === 'H') { x = ox + n(); path.lineTo(x, y); }
      else if (C === 'V') { y = (rel ? y : 0) + n(); path.lineTo(x, y); }
      else if (C === 'C') { var a = ox + n(), b = oy + n(), c = ox + n(), e = oy + n(); x = ox + n(); y = oy + n(); path.bezierCurveTo(a, b, c, e, x, y); }
      else if (C === 'Q') { var qa = ox + n(), qb = oy + n(); x = ox + n(); y = oy + n(); path.quadraticCurveTo(qa, qb, x, y); }
      else if (C === 'Z') { path.closePath(); x = sx; y = sy; }
      else if (C === 'A') { n(); n(); n(); n(); n(); x = ox + n(); y = oy + n(); path.lineTo(x, y); }
      else if (C === 'S' || C === 'T') { if (C === 'S') { n(); n(); } x = ox + n(); y = oy + n(); path.lineTo(x, y); }
      else i++;
    }
  }
  function Ctx(el){
    this.canvas = el;
    this.__el = el;
    this.__s = { fillStyle: '#000000', strokeStyle: '#000000', lineWidth: 1, lineCap: 'butt', lineJoin: 'miter',
      miterLimit: 10, globalAlpha: 1, globalCompositeOperation: 'source-over', font: '10px sans-serif',
      textAlign: 'start', textBaseline: 'alphabetic', lineDashOffset: 0, imageSmoothingEnabled: true,
      imageSmoothingQuality: 'low', shadowColor: 'rgba(0, 0, 0, 0)', shadowBlur: 0, shadowOffsetX: 0, shadowOffsetY: 0,
      filter: 'none', direction: 'ltr', letterSpacing: '0px', fontKerning: 'auto' };
    this.__stack = [];
    this.__dash = [];
  }
  var CP = Ctx.prototype;
  function cv(ctx, op){ var a = [ctx.__el, op]; for (var i = 2; i < arguments.length; i++) a.push(arguments[i]); return __cv.apply(null, a); }
  function style(ctx, which, v){
    if (v instanceof CanvasGradient) { cv(ctx, 22, which, 1, v.__g); return true; }
    if (v instanceof CanvasPattern) { cv(ctx, 22, which, 2, v.__p); return true; }
    return !!cv(ctx, 22, which, 0, String(v));
  }
  function prop(name, set){
    Object.defineProperty(CP, name, { configurable: true, enumerable: true,
      get: function(){ return this.__s[name]; },
      set: function(v){ if (set.call(this, v) !== false) this.__s[name] = v; } });
  }
  prop('fillStyle', function(v){ return style(this, 0, v); });
  prop('strokeStyle', function(v){ return style(this, 1, v); });
  prop('lineWidth', function(v){ v = +v; if (!(v > 0) || !isFinite(v)) return false; cv(this, 26, v); });
  prop('lineCap', function(v){ if (!(v in CAP)) return false; cv(this, 27, CAP[v]); });
  prop('lineJoin', function(v){ if (!(v in JOIN)) return false; cv(this, 28, JOIN[v]); });
  prop('miterLimit', function(v){ v = +v; if (!(v > 0)) return false; cv(this, 29, v); });
  prop('globalAlpha', function(v){ v = +v; if (!(v >= 0 && v <= 1)) return false; cv(this, 30, v); });
  prop('globalCompositeOperation', function(v){ if (!(v in COMP)) return false; cv(this, 31, COMP[v]); });
  prop('font', function(v){ cv(this, 32, String(v)); });
  prop('textAlign', function(v){ if (!(v in ALIGN)) return false; cv(this, 33, ALIGN[v]); });
  prop('textBaseline', function(v){ if (!(v in BASE)) return false; cv(this, 34, BASE[v]); });
  prop('lineDashOffset', function(v){ v = +v; if (!isFinite(v)) return false; cv(this, 44, v); });
  prop('imageSmoothingEnabled', function(v){ cv(this, 47, v ? 1 : 0); });
  ['imageSmoothingQuality', 'shadowColor', 'shadowBlur', 'shadowOffsetX', 'shadowOffsetY', 'filter', 'direction',
   'letterSpacing', 'fontKerning'].forEach(function(n){ prop(n, function(){}); });
  CP.fillRect = function(x, y, w, h){ cv(this, 1, +x, +y, +w, +h); };
  CP.strokeRect = function(x, y, w, h){ cv(this, 2, +x, +y, +w, +h); };
  CP.clearRect = function(x, y, w, h){ cv(this, 3, +x, +y, +w, +h); };
  CP.beginPath = function(){ cv(this, 4); };
  CP.moveTo = function(x, y){ cv(this, 5, +x, +y); };
  CP.lineTo = function(x, y){ cv(this, 6, +x, +y); };
  CP.closePath = function(){ cv(this, 7); };
  CP.rect = function(x, y, w, h){ cv(this, 8, +x, +y, +w, +h); };
  CP.roundRect = function(x, y, w, h, r){ var rr = r; if (rr && typeof rr === 'object') rr = rr.length !== undefined ? rr[0] : rr.x; if (rr && typeof rr === 'object') rr = rr.x || 0; cv(this, 46, +x, +y, +w, +h, +(rr || 0)); };
  CP.arc = function(x, y, r, a0, a1, ccw){ cv(this, 9, +x, +y, +r, +a0, +a1, ccw ? 1 : 0); };
  CP.arcTo = function(x1, y1, x2, y2, r){ cv(this, 10, +x1, +y1, +x2, +y2, +r); };
  CP.quadraticCurveTo = function(a, b, x, y){ cv(this, 11, +a, +b, +x, +y); };
  CP.bezierCurveTo = function(a, b, c, d, x, y){ cv(this, 12, +a, +b, +c, +d, +x, +y); };
  CP.ellipse = function(x, y, rx, ry, rot, a0, a1, ccw){ cv(this, 13, +x, +y, +rx, +ry, +rot, +a0, +a1, ccw ? 1 : 0); };
  function rule(r){ return r === 'evenodd' ? 1 : 0; }
  function usePath(ctx, p){ cv(ctx, 4); for (var i = 0; i < p.__ops.length; i++) ctx[p.__ops[i][0]].apply(ctx, p.__ops[i][1]); }
  CP.fill = function(a, b){ if (a instanceof Path2D) { usePath(this, a); cv(this, 14, rule(b)); } else cv(this, 14, rule(a)); };
  CP.stroke = function(a){ if (a instanceof Path2D) usePath(this, a); cv(this, 15); };
  CP.clip = function(a, b){ if (a instanceof Path2D) { usePath(this, a); cv(this, 16, rule(b)); } else cv(this, 16, rule(a)); };
  CP.isPointInPath = function(a, b, c, d){ if (a instanceof Path2D) { usePath(this, a); return cv(this, 45, +b, +c, rule(d)); } return cv(this, 45, +a, +b, rule(c)); };
  CP.isPointInStroke = function(a, b, c){ if (a instanceof Path2D) { usePath(this, a); return cv(this, 50, +b, +c); } return cv(this, 50, +a, +b); };
  CP.save = function(){ this.__stack.push([Object.assign({}, this.__s), this.__dash.slice()]); cv(this, 17); };
  CP.restore = function(){ var s = this.__stack.pop(); if (!s) return; this.__s = s[0]; this.__dash = s[1]; cv(this, 18); };
  CP.reset = function(){ var el = this.__el; Ctx.call(this, el); cv(this, 49); };
  CP.translate = function(x, y){ cv(this, 19, 1, 0, 0, 1, +x, +y); };
  CP.scale = function(x, y){ cv(this, 19, +x, 0, 0, +y, 0, 0); };
  CP.rotate = function(a){ a = +a; var c = Math.cos(a), s = Math.sin(a); cv(this, 19, c, s, -s, c, 0, 0); };
  CP.transform = function(a, b, c, d, e, f){ cv(this, 19, +a, +b, +c, +d, +e, +f); };
  CP.setTransform = function(a, b, c, d, e, f){
    if (a === undefined) { cv(this, 21); return; }
    if (typeof a === 'object') { var m = a; cv(this, 20, m.a !== undefined ? m.a : m.m11, m.b !== undefined ? m.b : m.m12, m.c !== undefined ? m.c : m.m21, m.d !== undefined ? m.d : m.m22, m.e !== undefined ? m.e : m.m41, m.f !== undefined ? m.f : m.m42); return; }
    cv(this, 20, +a, +b, +c, +d, +e, +f);
  };
  CP.resetTransform = function(){ cv(this, 21); };
  CP.getTransform = function(){ var m = cv(this, 48); if (typeof DOMMatrix === 'function') { try { return new DOMMatrix([m.a, m.b, m.c, m.d, m.e, m.f]); } catch (x) {} } m.is2D = true; return m; };
  CP.createLinearGradient = function(x0, y0, x1, y1){ return new CanvasGradient(__cv(null, 23, 0, +x0, +y0, +x1, +y1)); };
  CP.createRadialGradient = function(x0, y0, r0, x1, y1, r1){ return new CanvasGradient(__cv(null, 23, 1, +x0, +y0, +r0, +x1, +y1, +r1)); };
  CP.createConicGradient = function(a, x, y){ return new CanvasGradient(__cv(null, 23, 0, x - 1, y, x + 1, y)); };
  CP.createPattern = function(img, rep){
    rep = rep || 'repeat';
    var id = cv(this, 25, img, rep === 'repeat' || rep === 'repeat-x' ? 1 : 0, rep === 'repeat' || rep === 'repeat-y' ? 1 : 0);
    return id >= 0 ? new CanvasPattern(id) : null;
  };
  CP.fillText = function(t, x, y, mw){ cv(this, 35, String(t), +x, +y, mw === undefined ? undefined : +mw); };
  CP.strokeText = function(t, x, y, mw){ cv(this, 36, String(t), +x, +y, mw === undefined ? undefined : +mw); };
  CP.measureText = function(t){ return cv(this, 37, String(t)); };
  CP.drawImage = function(img){
    var a = Array.prototype.slice.call(arguments, 1).map(Number);
    if (img && img.tagName === undefined && img.canvas && img.canvas.tagName) img = img.canvas;   /* (a context: its canvas) */
    if (!img || (a.length !== 2 && a.length !== 4 && a.length !== 8)) throw new TypeError('drawImage: bad arguments');
    var args = [this.__el, 38, img, a.length].concat(a);
    __cv.apply(null, args);
  };
  CP.setLineDash = function(d){ if (!d || typeof d.length !== 'number') return; var v = []; for (var i = 0; i < d.length; i++) { var x = +d[i]; if (!(x >= 0) || !isFinite(x)) return; v.push(x); }
    this.__dash = v.length & 1 ? v.concat(v) : v; __cv.apply(null, [this.__el, 43, v.length].concat(v)); };
  CP.getLineDash = function(){ return this.__dash.slice(); };
  CP.createImageData = function(w, h){ if (w && w.width) return new ImageData(w.width, w.height); return new ImageData(Math.abs(w | 0), Math.abs(h | 0)); };
  CP.getImageData = function(x, y, w, h){
    w = w | 0; h = h | 0;
    if (w < 0) { x += w; w = -w; }
    if (h < 0) { y += h; h = -h; }
    var s = cv(this, 39, x | 0, y | 0, w, h), bin = atob(s || ''), d = new Uint8ClampedArray(w * h * 4);
    for (var i = 0, n = Math.min(bin.length, d.length); i < n; i++) d[i] = bin.charCodeAt(i);
    return new ImageData(d, w, h);
  };
  CP.putImageData = function(img, dx, dy, sx, sy, sw, sh){
    /* the bytes as hex (plain ASCII crosses into C the same in every engine) */
    var d = img.data, parts = new Array(d.length);
    for (var i = 0; i < d.length; i++) parts[i] = HEX[d[i] & 255];
    var b64 = parts.join('');
    if (sx === undefined) cv(this, 40, b64, img.width, img.height, dx | 0, dy | 0, 0, 0, img.width, img.height);
    else cv(this, 40, b64, img.width, img.height, dx | 0, dy | 0, sx | 0, sy | 0, sw | 0, sh | 0);
  };
  CP.getContextAttributes = function(){ return { alpha: true, desynchronized: false, colorSpace: 'srgb', willReadFrequently: false }; };
  CP.isContextLost = function(){ return false; };
  CP.drawFocusIfNeeded = function(){};
  CP.scrollPathIntoView = function(){};
  EPR.getContext = function(type, opts){
    if (this.tagName !== 'CANVAS') return undefined;
    type = String(type).toLowerCase();
    if (type === '2d') return this.__ctx2d || (this.__ctx2d = new Ctx(this));
    if ((type === 'webgl' || type === 'experimental-webgl' || type === 'webgl2') && typeof W.__webglContext === 'function')
      return W.__webglContext(this, type, opts);
    return null;
  };
  EPR.toDataURL = function(type){ if (this.tagName !== 'CANVAS') return undefined; if (!this.__ctx2d) this.getContext('2d'); return __cv(this, 41); };
  EPR.toBlob = function(cb, type){
    var url = this.toDataURL(type), bin = atob(url.split(',')[1] || ''), u = new Uint8Array(bin.length);
    for (var i = 0; i < bin.length; i++) u[i] = bin.charCodeAt(i);
    var blob = typeof Blob === 'function' ? new Blob([u], { type: 'image/png' }) : null;
    setTimeout(function(){ cb(blob); }, 0);
  };
  EPR.captureStream = function(){ return null; };
  EPR.transferControlToOffscreen = function(){ throw new Error('OffscreenCanvas is not supported'); };
  W.CanvasRenderingContext2D = Ctx;
  W.CanvasGradient = CanvasGradient;
  W.CanvasPattern = CanvasPattern;
  W.Path2D = Path2D;
  if (typeof W.ImageData === 'undefined') W.ImageData = ImageData;
  /* requestAnimationFrame callbacks get the time, as elsewhere */
  var raf = W.requestAnimationFrame;
  if (raf) W.requestAnimationFrame = function(cb){ return raf(function(){ cb(P.now()); }); };
})();
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
      m.url = u; m.id = __media_open(u, el); m.ready = 0; m.dur = NaN; m.pos = 0; m.ended = false; m.err = null;
      if (!m.touched) { m.touched = true; m.muted = el.hasAttribute('muted'); }
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
    return /^(audio\/(mpeg|mp3|mpeg3|x-mpeg|wav|wave|x-wav|flac|x-flac|aac|mp4|x-m4a|ogg|opus|webm)|video\/(mp4|webm|ogg|quicktime|x-matroska|x-m4v))/.test(t) ? 'probably' : '';
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
  prop('muted', function(m){ return m.touched ? m.muted : this.hasAttribute('muted'); }, function(m, v){ m.touched = true; m.muted = !!v; pushSet(this); fire(this, 'volumechange'); });
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
  /* <video autoplay> / <audio autoplay> in the document: they start once it is parsed
   * (a muted one plays without asking, as in other browsers) */
  function autoplay(){
    var list = document.querySelectorAll('video[autoplay], audio[autoplay]');
    for (var i = 0; i < list.length; i++) { var el = list[i]; if (!el.__ms || el.__ms.url === '') ensure(el); }
  }
  document.addEventListener('DOMContentLoaded', autoplay);
  setTimeout(autoplay, 0);
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

/* uploads: the file chosen in an <input type=file> is a File read from the
 * disk when sent (FormData + fetch / XMLHttpRequest) or read (text(), FileReader) */
window.__blobStr = function(b){ return b && b._path !== undefined ? __utf8_decode(__readFile(b._path) || new Uint8Array(0)) : (b && b._s !== undefined ? b._s : String(b)); };
window.FileList = class FileList { constructor(a){ a = a || []; for (var i = 0; i < a.length; i++) this[i] = a[i]; this.length = a.length; }
  item(i){ return this[i] || null; } [Symbol.iterator](){ return Array.prototype.slice.call(this)[Symbol.iterator](); } };
(function(){
  var BP = Blob.prototype;
  BP.text = function(){ return Promise.resolve(__blobStr(this)); };
  BP.arrayBuffer = function(){ return Promise.resolve((this._path !== undefined ? (__readFile(this._path) || new Uint8Array(0)) : __utf8_encode(this._s)).buffer); };
  BP.bytes = function(){ return Promise.resolve(this._path !== undefined ? (__readFile(this._path) || new Uint8Array(0)) : __utf8_encode(this._s)); };
  BP.slice = function(a, b, t){ var r = new Blob([]); r._s = __blobStr(this).slice(a, b); r.type = t || ''; return r; };
  BP.stream = function(){ var me = this; return { getReader: function(){ var done = false; return { read: function(){
    if (done) return Promise.resolve({done: true, value: undefined}); done = true;
    return me.bytes().then(function(v){ return {done: false, value: v}; }); }, releaseLock: function(){}, cancel: function(){ return Promise.resolve(); } }; } }; };
  Object.defineProperty(Element.prototype, 'files', { configurable: true,
    get: function(){ if (this.tagName !== 'INPUT' || (this.getAttribute('type') || '').toLowerCase() !== 'file') return undefined;
      return this.__files || (this.__files = new FileList()); },
    set: function(v){ this.__files = v; } });
})();
window.__setFiles = function(el, path, name, size, type){
  var f = Object.create(File.prototype); f._path = path; f.name = name; f.type = type || ''; f.lastModified = Date.now(); f.webkitRelativePath = '';
  f._size = size;
  el.__files = new FileList([f]);
  el.dispatchEvent(new Event('input', {bubbles: true}));
  el.dispatchEvent(new Event('change', {bubbles: true}));
};
/* document.implementation: detached documents (jQuery builds HTML in one) */
try { if (!document.implementation || !document.implementation.createHTMLDocument) document.implementation = {
  createHTMLDocument: function(t){ var d = new DOMParser().parseFromString(""); d.title = t || ""; return d; },
  createDocument: function(){ return new DOMParser().parseFromString(""); },
  createDocumentType: function(){ return null; }, hasFeature: function(){ return true; } }; } catch (e) {}
try { if (typeof document.close !== 'function') document.close = function(){}; if (typeof document.open !== 'function') document.open = function(){ return document; }; } catch (e) {}
