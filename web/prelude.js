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
  prop('shadowColor', function(v){ return !!cv(this, 51, String(v)); });
  prop('shadowBlur', function(v){ v = +v; if (!(v >= 0) || !isFinite(v)) return false; cv(this, 52, 0, v); });
  prop('shadowOffsetX', function(v){ v = +v; if (!isFinite(v)) return false; cv(this, 52, 1, v); });
  prop('shadowOffsetY', function(v){ v = +v; if (!isFinite(v)) return false; cv(this, 52, 2, v); });
  ['imageSmoothingQuality', 'filter', 'direction', 'letterSpacing', 'fontKerning'].forEach(function(n){ prop(n, function(){}); });
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
  CP.createConicGradient = function(a, x, y){ return new CanvasGradient(__cv(null, 23, 2, +x, +y, +a)); };
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
    if (type === '2d') return this.__gl ? null : this.__ctx2d || (this.__ctx2d = new Ctx(this));
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
/* WebGL 1 and 2 (getContext('webgl' / 'webgl2')): calls recorded by web/webgl.c through
 * __gl(canvas, op, ...) and played on the GPU by the browser's GPU worker
 * (apps/webgl, Mesa). Frames are shown after the task that drew them. */
(function(){
  if (typeof __gl !== 'function') return;
  var C = {};
  ('DEPTH_BUFFER_BIT=256 STENCIL_BUFFER_BIT=1024 COLOR_BUFFER_BIT=16384 POINTS=0 LINES=1 LINE_LOOP=2 LINE_STRIP=3 ' +
   'TRIANGLES=4 TRIANGLE_STRIP=5 TRIANGLE_FAN=6 ZERO=0 ONE=1 SRC_COLOR=768 ONE_MINUS_SRC_COLOR=769 SRC_ALPHA=770 ' +
   'ONE_MINUS_SRC_ALPHA=771 DST_ALPHA=772 ONE_MINUS_DST_ALPHA=773 DST_COLOR=774 ONE_MINUS_DST_COLOR=775 ' +
   'SRC_ALPHA_SATURATE=776 FUNC_ADD=32774 BLEND_EQUATION=32777 BLEND_EQUATION_RGB=32777 BLEND_EQUATION_ALPHA=34877 ' +
   'FUNC_SUBTRACT=32778 FUNC_REVERSE_SUBTRACT=32779 BLEND_DST_RGB=32968 BLEND_SRC_RGB=32969 BLEND_DST_ALPHA=32970 ' +
   'BLEND_SRC_ALPHA=32971 CONSTANT_COLOR=32769 ONE_MINUS_CONSTANT_COLOR=32770 CONSTANT_ALPHA=32771 ' +
   'ONE_MINUS_CONSTANT_ALPHA=32772 BLEND_COLOR=32773 ARRAY_BUFFER=34962 ELEMENT_ARRAY_BUFFER=34963 ' +
   'ARRAY_BUFFER_BINDING=34964 ELEMENT_ARRAY_BUFFER_BINDING=34965 STREAM_DRAW=35040 STATIC_DRAW=35044 ' +
   'DYNAMIC_DRAW=35048 BUFFER_SIZE=34660 BUFFER_USAGE=34661 CURRENT_VERTEX_ATTRIB=34342 FRONT=1028 BACK=1029 ' +
   'FRONT_AND_BACK=1032 CULL_FACE=2884 BLEND=3042 DITHER=3024 STENCIL_TEST=2960 DEPTH_TEST=2929 SCISSOR_TEST=3089 ' +
   'POLYGON_OFFSET_FILL=32823 SAMPLE_ALPHA_TO_COVERAGE=32926 SAMPLE_COVERAGE=32928 NO_ERROR=0 INVALID_ENUM=1280 ' +
   'INVALID_VALUE=1281 INVALID_OPERATION=1282 OUT_OF_MEMORY=1285 CW=2304 CCW=2305 LINE_WIDTH=2849 ' +
   'ALIASED_POINT_SIZE_RANGE=33901 ALIASED_LINE_WIDTH_RANGE=33902 CULL_FACE_MODE=2885 FRONT_FACE=2886 ' +
   'DEPTH_RANGE=2928 DEPTH_WRITEMASK=2930 DEPTH_CLEAR_VALUE=2931 DEPTH_FUNC=2932 STENCIL_CLEAR_VALUE=2961 ' +
   'STENCIL_FUNC=2962 STENCIL_FAIL=2964 STENCIL_PASS_DEPTH_FAIL=2965 STENCIL_PASS_DEPTH_PASS=2966 STENCIL_REF=2967 ' +
   'STENCIL_VALUE_MASK=2963 STENCIL_WRITEMASK=2968 STENCIL_BACK_FUNC=34816 STENCIL_BACK_FAIL=34817 ' +
   'STENCIL_BACK_PASS_DEPTH_FAIL=34818 STENCIL_BACK_PASS_DEPTH_PASS=34819 STENCIL_BACK_REF=36003 ' +
   'STENCIL_BACK_VALUE_MASK=36004 STENCIL_BACK_WRITEMASK=36005 VIEWPORT=2978 SCISSOR_BOX=3088 COLOR_CLEAR_VALUE=3106 ' +
   'COLOR_WRITEMASK=3107 UNPACK_ALIGNMENT=3317 PACK_ALIGNMENT=3333 MAX_TEXTURE_SIZE=3379 MAX_VIEWPORT_DIMS=3386 ' +
   'SUBPIXEL_BITS=3408 RED_BITS=3410 GREEN_BITS=3411 BLUE_BITS=3412 ALPHA_BITS=3413 DEPTH_BITS=3414 STENCIL_BITS=3415 ' +
   'POLYGON_OFFSET_UNITS=10752 POLYGON_OFFSET_FACTOR=32824 TEXTURE_BINDING_2D=32873 SAMPLE_BUFFERS=32936 SAMPLES=32937 ' +
   'SAMPLE_COVERAGE_VALUE=32938 SAMPLE_COVERAGE_INVERT=32939 COMPRESSED_TEXTURE_FORMATS=34467 DONT_CARE=4352 ' +
   'FASTEST=4353 NICEST=4354 GENERATE_MIPMAP_HINT=33170 BYTE=5120 UNSIGNED_BYTE=5121 SHORT=5122 UNSIGNED_SHORT=5123 ' +
   'INT=5124 UNSIGNED_INT=5125 FLOAT=5126 DEPTH_COMPONENT=6402 ALPHA=6406 RGB=6407 RGBA=6408 LUMINANCE=6409 ' +
   'LUMINANCE_ALPHA=6410 UNSIGNED_SHORT_4_4_4_4=32819 UNSIGNED_SHORT_5_5_5_1=32820 UNSIGNED_SHORT_5_6_5=33635 ' +
   'FRAGMENT_SHADER=35632 VERTEX_SHADER=35633 MAX_VERTEX_ATTRIBS=34921 MAX_VERTEX_UNIFORM_VECTORS=36347 ' +
   'MAX_VARYING_VECTORS=36348 MAX_COMBINED_TEXTURE_IMAGE_UNITS=35661 MAX_VERTEX_TEXTURE_IMAGE_UNITS=35660 ' +
   'MAX_TEXTURE_IMAGE_UNITS=34930 MAX_FRAGMENT_UNIFORM_VECTORS=36349 SHADER_TYPE=35663 DELETE_STATUS=35712 ' +
   'LINK_STATUS=35714 VALIDATE_STATUS=35715 ATTACHED_SHADERS=35717 ACTIVE_UNIFORMS=35718 ACTIVE_ATTRIBUTES=35721 ' +
   'SHADING_LANGUAGE_VERSION=35724 CURRENT_PROGRAM=35725 NEVER=512 LESS=513 EQUAL=514 LEQUAL=515 GREATER=516 ' +
   'NOTEQUAL=517 GEQUAL=518 ALWAYS=519 KEEP=7680 REPLACE=7681 INCR=7682 DECR=7683 INVERT=5386 INCR_WRAP=34055 ' +
   'DECR_WRAP=34056 VENDOR=7936 RENDERER=7937 VERSION=7938 NEAREST=9728 LINEAR=9729 NEAREST_MIPMAP_NEAREST=9984 ' +
   'LINEAR_MIPMAP_NEAREST=9985 NEAREST_MIPMAP_LINEAR=9986 LINEAR_MIPMAP_LINEAR=9987 TEXTURE_MAG_FILTER=10240 ' +
   'TEXTURE_MIN_FILTER=10241 TEXTURE_WRAP_S=10242 TEXTURE_WRAP_T=10243 TEXTURE_2D=3553 TEXTURE=5890 ' +
   'TEXTURE_CUBE_MAP=34067 TEXTURE_BINDING_CUBE_MAP=34068 TEXTURE_CUBE_MAP_POSITIVE_X=34069 ' +
   'TEXTURE_CUBE_MAP_NEGATIVE_X=34070 TEXTURE_CUBE_MAP_POSITIVE_Y=34071 TEXTURE_CUBE_MAP_NEGATIVE_Y=34072 ' +
   'TEXTURE_CUBE_MAP_POSITIVE_Z=34073 TEXTURE_CUBE_MAP_NEGATIVE_Z=34074 MAX_CUBE_MAP_TEXTURE_SIZE=34076 ' +
   'TEXTURE0=33984 ACTIVE_TEXTURE=34016 REPEAT=10497 CLAMP_TO_EDGE=33071 MIRRORED_REPEAT=33648 FLOAT_VEC2=35664 ' +
   'FLOAT_VEC3=35665 FLOAT_VEC4=35666 INT_VEC2=35667 INT_VEC3=35668 INT_VEC4=35669 BOOL=35670 BOOL_VEC2=35671 ' +
   'BOOL_VEC3=35672 BOOL_VEC4=35673 FLOAT_MAT2=35674 FLOAT_MAT3=35675 FLOAT_MAT4=35676 SAMPLER_2D=35678 ' +
   'SAMPLER_CUBE=35680 VERTEX_ATTRIB_ARRAY_ENABLED=34338 VERTEX_ATTRIB_ARRAY_SIZE=34339 ' +
   'VERTEX_ATTRIB_ARRAY_STRIDE=34340 VERTEX_ATTRIB_ARRAY_TYPE=34341 VERTEX_ATTRIB_ARRAY_NORMALIZED=34922 ' +
   'VERTEX_ATTRIB_ARRAY_POINTER=34373 VERTEX_ATTRIB_ARRAY_BUFFER_BINDING=34975 IMPLEMENTATION_COLOR_READ_TYPE=35738 ' +
   'IMPLEMENTATION_COLOR_READ_FORMAT=35739 COMPILE_STATUS=35713 LOW_FLOAT=36336 MEDIUM_FLOAT=36337 HIGH_FLOAT=36338 ' +
   'LOW_INT=36339 MEDIUM_INT=36340 HIGH_INT=36341 FRAMEBUFFER=36160 RENDERBUFFER=36161 RGBA4=32854 RGB5_A1=32855 ' +
   'RGB565=36194 DEPTH_COMPONENT16=33189 STENCIL_INDEX8=36168 DEPTH_STENCIL=34041 RENDERBUFFER_WIDTH=36162 ' +
   'RENDERBUFFER_HEIGHT=36163 RENDERBUFFER_INTERNAL_FORMAT=36164 COLOR_ATTACHMENT0=36064 DEPTH_ATTACHMENT=36096 ' +
   'STENCIL_ATTACHMENT=36128 DEPTH_STENCIL_ATTACHMENT=33306 NONE=0 FRAMEBUFFER_COMPLETE=36053 ' +
   'FRAMEBUFFER_INCOMPLETE_ATTACHMENT=36054 FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT=36055 ' +
   'FRAMEBUFFER_INCOMPLETE_DIMENSIONS=36057 FRAMEBUFFER_UNSUPPORTED=36061 FRAMEBUFFER_BINDING=36006 ' +
   'RENDERBUFFER_BINDING=36007 MAX_RENDERBUFFER_SIZE=34024 INVALID_FRAMEBUFFER_OPERATION=1286 ' +
   'UNPACK_FLIP_Y_WEBGL=37440 UNPACK_PREMULTIPLY_ALPHA_WEBGL=37441 CONTEXT_LOST_WEBGL=37442 ' +
   'UNPACK_COLORSPACE_CONVERSION_WEBGL=37443 BROWSER_DEFAULT_WEBGL=37444').split(' ').forEach(function(kv){
    var i = kv.indexOf('='); C[kv.slice(0, i)] = +kv.slice(i + 1); });
  /* the command numbers (web/webgl_ops.h) */
  var N = 0, O = {};
  ['RESIZE', 'CREATE', 'DELETE', 'SHADER_SOURCE', 'COMPILE', 'ATTACH', 'DETACH', 'LINK', 'USE', 'VALIDATE',
   'SHADER_PARAM', 'PROGRAM_PARAM', 'SHADER_LOG', 'PROGRAM_LOG', 'ATTRIB_LOC', 'BIND_ATTRIB_LOC', 'UNIFORM_LOC',
   'ACTIVE', 'UNIFORM_F', 'UNIFORM_I', 'UNIFORM_FV', 'UNIFORM_IV', 'UNIFORM_MAT', 'ATTRIB_PTR', 'ENABLE_ATTRIB',
   'DISABLE_ATTRIB', 'ATTRIB_F', 'BIND_BUFFER', 'BUFFER_DATA', 'BUFFER_SUB', 'DRAW_ARRAYS', 'DRAW_ELEMENTS',
   'VIEWPORT', 'SCISSOR', 'CLEAR_COLOR', 'CLEAR_DEPTH', 'CLEAR_STENCIL', 'CLEAR', 'ENABLE', 'DISABLE',
   'BLEND_FUNC', 'BLEND_EQ', 'BLEND_COLOR', 'DEPTH_FUNC', 'DEPTH_MASK', 'DEPTH_RANGE', 'COLOR_MASK', 'CULL_FACE',
   'FRONT_FACE', 'LINE_WIDTH', 'POLYGON_OFFSET', 'STENCIL_FUNC', 'STENCIL_OP', 'STENCIL_MASK', 'HINT',
   'PIXEL_STORE', 'ACTIVE_TEXTURE', 'BIND_TEXTURE', 'TEX_PARAM_I', 'TEX_PARAM_F', 'TEX_IMAGE', 'TEX_SUB_IMAGE',
   'GEN_MIPMAP', 'BIND_FRAMEBUFFER', 'FB_TEXTURE', 'FB_RENDERBUFFER', 'CHECK_FB', 'BIND_RENDERBUFFER',
   'RB_STORAGE', 'READ_PIXELS', 'GET_ERROR', 'GET_PARAM', 'IS_ENABLED', 'GET_STRING', 'FINISH', 'COPY_TEX_IMAGE',
   'COPY_TEX_SUB_IMAGE', 'SAMPLE_COVERAGE', 'STENCIL_MASK_SEP_DUMMY', 'DRAW_ARRAYS_INST', 'DRAW_ELEMENTS_INST',
   'ATTRIB_DIVISOR', 'BIND_VAO', 'TEX_IMAGE3D', 'TEX_SUB_IMAGE3D', 'TEX_STORAGE2D', 'TEX_STORAGE3D', 'DRAW_BUFFERS',
   'READ_BUFFER', 'BLIT_FB', 'RB_STORAGE_MS', 'FB_TEX_LAYER', 'BIND_BUFFER_BASE', 'BIND_BUFFER_RANGE',
   'UNIFORM_BLOCK_INDEX', 'UNIFORM_BLOCK_BINDING', 'UNIFORM_BLOCK_PARAM', 'UNIFORM_UI', 'UNIFORM_UIV', 'UNIFORM_MAT_NS',
   'ATTRIB_IPTR', 'ATTRIB_I', 'CLEAR_BUFFER_FV', 'CLEAR_BUFFER_IV', 'CLEAR_BUFFER_FI', 'DRAW_RANGE_ELEMENTS',
   'BIND_SAMPLER', 'SAMPLER_PARAM_I', 'SAMPLER_PARAM_F', 'FRAG_DATA_LOC', 'COPY_BUFFER_SUB', 'GET_BUFFER_SUB',
   'BEGIN_QUERY', 'END_QUERY', 'QUERY_RESULT', 'BIND_TF', 'BEGIN_TF', 'END_TF', 'TF_VARYINGS', 'PAUSE_TF',
   'RESUME_TF'].forEach(function(n){ O[n] = ++N; });
  var X_OPEN = 200, X_CREATE = 201, X_PRESENT = 202, X_TEX_EL = 203, X_TEX_SUB_EL = 204, X_UNPACK = 205, X_TYPED = 206, X_LOST = 207;
  var K = { buffer: 0, texture: 1, framebuffer: 2, renderbuffer: 3, program: 4, shader: 5, vao: 6, query: 7, sampler: 8, tf: 9 };
  var HEX = []; for (var hx = 0; hx < 256; hx++) HEX.push((hx < 16 ? '0' : '') + hx.toString(16));
  var direct = null;                                 /* can C read our typed arrays itself? */
  function bytesOf(cv, d){
    if (d === null || d === undefined) return null;
    var u = ArrayBuffer.isView(d) ? new Uint8Array(d.buffer, d.byteOffset, d.byteLength) : d instanceof ArrayBuffer ? new Uint8Array(d) : null;
    if (!u) return null;
    if (direct === null) direct = !!__gl(cv, X_TYPED, new Uint8Array(1));
    if (direct) return u;
    var s = new Array(u.length);
    for (var i = 0; i < u.length; i++) s[i] = HEX[u[i]];
    return s.join('');
  }
  function f32(cv, v){ return bytesOf(cv, v instanceof Float32Array ? v : new Float32Array(v)); }
  function i32(cv, v){ return bytesOf(cv, v instanceof Int32Array ? v : new Int32Array(v)); }
  function Obj(kind, id){ this.__k = kind; this.__id = id; this.__dead = false; }
  function WebGLBuffer(){} function WebGLTexture(){} function WebGLProgram(){} function WebGLShader(){}
  function WebGLFramebuffer(){} function WebGLRenderbuffer(){} function WebGLUniformLocation(){}
  function WebGLActiveInfo(o){ this.size = o.size; this.type = o.type; this.name = o.name; }
  function WebGLShaderPrecisionFormat(r0, r1, p){ this.rangeMin = r0; this.rangeMax = r1; this.precision = p; }
  function WebGLVertexArrayObject(){} function WebGLQuery(){} function WebGLSampler(){} function WebGLTransformFeedback(){}
  function WebGLSync(){}
  var CTOR = { buffer: WebGLBuffer, texture: WebGLTexture, framebuffer: WebGLFramebuffer, renderbuffer: WebGLRenderbuffer,
               program: WebGLProgram, shader: WebGLShader, vao: WebGLVertexArrayObject, query: WebGLQuery,
               sampler: WebGLSampler, tf: WebGLTransformFeedback };
  function idOf(o){ return o && !o.__dead && o.__id ? o.__id : 0; }
  function GL(cv, opts){
    this.canvas = cv; this.__cv = cv; this.__opts = opts; this.__err = 0; this.__flipY = 0; this.__premul = 0; this.__shown = false;
    this.drawingBufferColorSpace = 'srgb'; this.unpackColorSpace = 'srgb';
  }
  var GP = GL.prototype;
  for (var key in C) GP[key] = C[key];
  Object.defineProperty(GP, 'drawingBufferWidth', { get: function(){ return this.__cv.width; } });
  Object.defineProperty(GP, 'drawingBufferHeight', { get: function(){ return this.__cv.height; } });
  function g(gl, op){ var a = [gl.__cv, op]; for (var i = 2; i < arguments.length; i++) a.push(arguments[i]); var r = __gl.apply(null, a); return r; }
  function drew(gl){                                 /* shown after this task, as browsers do */
    if (gl.__pending) return;
    gl.__pending = true;
    setTimeout(function(){ gl.__pending = false; __gl(gl.__cv, X_PRESENT); }, 0);
  }
  function make(gl, kind, type){
    var F = CTOR[kind], o = new F();
    o.__k = kind; o.__id = g(gl, X_CREATE, K[kind], type || 0); o.__dead = false;
    return o.__id ? o : null;
  }
  GP.createBuffer = function(){ return make(this, 'buffer'); };
  GP.createTexture = function(){ return make(this, 'texture'); };
  GP.createFramebuffer = function(){ return make(this, 'framebuffer'); };
  GP.createRenderbuffer = function(){ return make(this, 'renderbuffer'); };
  GP.createProgram = function(){ return make(this, 'program'); };
  GP.createShader = function(t){ var s = make(this, 'shader', t); if (s) s.__type = t; return s; };
  function del(kind){ return function(o){ if (!o || o.__dead) return; g(this, O.DELETE, K[kind], o.__id); o.__dead = true; }; }
  GP.deleteBuffer = del('buffer'); GP.deleteTexture = del('texture'); GP.deleteFramebuffer = del('framebuffer');
  GP.deleteRenderbuffer = del('renderbuffer'); GP.deleteProgram = del('program'); GP.deleteShader = del('shader');
  GP.isBuffer = GP.isTexture = GP.isFramebuffer = GP.isRenderbuffer = GP.isProgram = GP.isShader = function(o){ return !!(o && !o.__dead && o.__id); };
  GP.shaderSource = function(s, src){ s.__src = String(src); g(this, O.SHADER_SOURCE, idOf(s), String(src)); };
  GP.getShaderSource = function(s){ return s && s.__src !== undefined ? s.__src : null; };
  GP.compileShader = function(s){ g(this, O.COMPILE, idOf(s)); };
  GP.attachShader = function(p, s){ g(this, O.ATTACH, idOf(p), idOf(s)); };
  GP.detachShader = function(p, s){ g(this, O.DETACH, idOf(p), idOf(s)); };
  GP.linkProgram = function(p){ g(this, O.LINK, idOf(p)); if (p) p.__locs = {}; };
  GP.useProgram = function(p){ g(this, O.USE, idOf(p)); this.__prog = p; };
  GP.validateProgram = function(p){ g(this, O.VALIDATE, idOf(p)); };
  GP.getShaderParameter = function(s, pn){ if (pn === C.SHADER_TYPE) return s.__type; var v = g(this, O.SHADER_PARAM, idOf(s), pn); return pn === C.COMPILE_STATUS || pn === C.DELETE_STATUS ? !!v : v; };
  GP.getProgramParameter = function(p, pn){ var v = g(this, O.PROGRAM_PARAM, idOf(p), pn); return pn === C.LINK_STATUS || pn === C.VALIDATE_STATUS || pn === C.DELETE_STATUS ? !!v : v; };
  GP.getShaderInfoLog = function(s){ return g(this, O.SHADER_LOG, idOf(s)) || ''; };
  GP.getProgramInfoLog = function(p){ return g(this, O.PROGRAM_LOG, idOf(p)) || ''; };
  GP.getAttribLocation = function(p, n){ return g(this, O.ATTRIB_LOC, idOf(p), String(n)); };
  GP.bindAttribLocation = function(p, i, n){ g(this, O.BIND_ATTRIB_LOC, idOf(p), i, String(n)); };
  GP.getUniformLocation = function(p, n){
    if (!p) return null;
    var c = p.__locs || (p.__locs = {});
    if (n in c) return c[n];
    var l = g(this, O.UNIFORM_LOC, idOf(p), String(n)), o = null;
    if (l >= 0) { o = new WebGLUniformLocation(); o.__l = l; o.__p = p; }
    return c[n] = o;
  };
  GP.getActiveAttrib = function(p, i){ var o = g(this, O.ACTIVE, idOf(p), 0, i); return o ? new WebGLActiveInfo(o) : null; };
  GP.getActiveUniform = function(p, i){ var o = g(this, O.ACTIVE, idOf(p), 1, i); return o ? new WebGLActiveInfo(o) : null; };
  GP.getAttachedShaders = function(){ return []; };
  GP.getShaderPrecisionFormat = function(st, pt){ return pt >= C.LOW_INT ? new WebGLShaderPrecisionFormat(31, 30, 0) : new WebGLShaderPrecisionFormat(127, 127, 23); };
  function loc(l){ return l ? l.__l : -1; }
  [1, 2, 3, 4].forEach(function(n){
    GP['uniform' + n + 'f'] = function(l){ if (!l) return; var a = [this, O.UNIFORM_F, loc(l), n]; for (var i = 1; i <= n; i++) a.push(+arguments[i]); g.apply(null, a); };
    GP['uniform' + n + 'i'] = function(l){ if (!l) return; var a = [this, O.UNIFORM_I, loc(l), n]; for (var i = 1; i <= n; i++) a.push(arguments[i] | 0); g.apply(null, a); };
    GP['uniform' + n + 'fv'] = function(l, v){ if (!l || !v) return; g(this, O.UNIFORM_FV, loc(l), n, (v.length / n) | 0, f32(this.__cv, v)); };
    GP['uniform' + n + 'iv'] = function(l, v){ if (!l || !v) return; g(this, O.UNIFORM_IV, loc(l), n, (v.length / n) | 0, i32(this.__cv, v)); };
  });
  [2, 3, 4].forEach(function(n){
    GP['uniformMatrix' + n + 'fv'] = function(l, tr, v){ if (!l || !v) return; g(this, O.UNIFORM_MAT, loc(l), n, (v.length / (n * n)) | 0, f32(this.__cv, v)); };
  });
  GP.vertexAttribPointer = function(i, size, type, norm, stride, off){ g(this, O.ATTRIB_PTR, i, size, type, norm ? 1 : 0, stride, off); };
  GP.enableVertexAttribArray = function(i){ g(this, O.ENABLE_ATTRIB, i); };
  GP.disableVertexAttribArray = function(i){ g(this, O.DISABLE_ATTRIB, i); };
  [1, 2, 3, 4].forEach(function(n){
    GP['vertexAttrib' + n + 'f'] = function(i){ var a = [this, O.ATTRIB_F, i, n]; for (var k = 1; k <= n; k++) a.push(+arguments[k]); g.apply(null, a); };
    GP['vertexAttrib' + n + 'fv'] = function(i, v){ var a = [this, O.ATTRIB_F, i, n]; for (var k = 0; k < n; k++) a.push(+v[k]); g.apply(null, a); };
  });
  GP.bindBuffer = function(t, b){ g(this, O.BIND_BUFFER, t, idOf(b)); };
  GP.bufferData = function(t, d, usage){
    if (typeof d === 'number') g(this, O.BUFFER_DATA, t, usage, d, null);
    else { var b = bytesOf(this.__cv, d); g(this, O.BUFFER_DATA, t, usage, d ? d.byteLength : 0, b); }
  };
  GP.bufferSubData = function(t, off, d){ g(this, O.BUFFER_SUB, t, off, bytesOf(this.__cv, d)); };
  GP.drawArrays = function(m, first, count){ g(this, O.DRAW_ARRAYS, m, first, count); drew(this); };
  GP.drawElements = function(m, count, type, off){ g(this, O.DRAW_ELEMENTS, m, count, type, off); drew(this); };
  GP.viewport = function(x, y, w, h){ g(this, O.VIEWPORT, x, y, w, h); };
  GP.scissor = function(x, y, w, h){ g(this, O.SCISSOR, x, y, w, h); };
  GP.clearColor = function(r, gg, b, a){ g(this, O.CLEAR_COLOR, +r, +gg, +b, +a); };
  GP.clearDepth = function(d){ g(this, O.CLEAR_DEPTH, +d); };
  GP.clearStencil = function(s){ g(this, O.CLEAR_STENCIL, s); };
  GP.clear = function(m){ g(this, O.CLEAR, m); drew(this); };
  GP.enable = function(c){ g(this, O.ENABLE, c); };
  GP.disable = function(c){ g(this, O.DISABLE, c); };
  GP.isEnabled = function(c){ return !!g(this, O.IS_ENABLED, c); };
  GP.blendFunc = function(s, d){ g(this, O.BLEND_FUNC, s, d, s, d); };
  GP.blendFuncSeparate = function(a, b, c, d){ g(this, O.BLEND_FUNC, a, b, c, d); };
  GP.blendEquation = function(m){ g(this, O.BLEND_EQ, m, m); };
  GP.blendEquationSeparate = function(a, b){ g(this, O.BLEND_EQ, a, b); };
  GP.blendColor = function(r, gg, b, a){ g(this, O.BLEND_COLOR, +r, +gg, +b, +a); };
  GP.depthFunc = function(f){ g(this, O.DEPTH_FUNC, f); };
  GP.depthMask = function(f){ g(this, O.DEPTH_MASK, f ? 1 : 0); };
  GP.depthRange = function(n, f){ g(this, O.DEPTH_RANGE, +n, +f); };
  GP.colorMask = function(r, gg, b, a){ g(this, O.COLOR_MASK, r ? 1 : 0, gg ? 1 : 0, b ? 1 : 0, a ? 1 : 0); };
  GP.cullFace = function(m){ g(this, O.CULL_FACE, m); };
  GP.frontFace = function(m){ g(this, O.FRONT_FACE, m); };
  GP.lineWidth = function(w){ g(this, O.LINE_WIDTH, +w); };
  GP.polygonOffset = function(f, u){ g(this, O.POLYGON_OFFSET, +f, +u); };
  GP.stencilFunc = function(f, r, m){ g(this, O.STENCIL_FUNC, C.FRONT_AND_BACK, f, r, m); };
  GP.stencilFuncSeparate = function(face, f, r, m){ g(this, O.STENCIL_FUNC, face, f, r, m); };
  GP.stencilOp = function(a, b, c){ g(this, O.STENCIL_OP, C.FRONT_AND_BACK, a, b, c); };
  GP.stencilOpSeparate = function(face, a, b, c){ g(this, O.STENCIL_OP, face, a, b, c); };
  GP.stencilMask = function(m){ g(this, O.STENCIL_MASK, C.FRONT_AND_BACK, m); };
  GP.stencilMaskSeparate = function(face, m){ g(this, O.STENCIL_MASK, face, m); };
  GP.hint = function(t, m){ g(this, O.HINT, t, m); };
  GP.sampleCoverage = function(v, inv){ g(this, O.SAMPLE_COVERAGE, +v, inv ? 1 : 0); };
  GP.pixelStorei = function(pn, v){
    if (pn === C.UNPACK_FLIP_Y_WEBGL) { this.__flipY = v ? 1 : 0; g(this, X_UNPACK, this.__flipY, this.__premul); }
    else if (pn === C.UNPACK_PREMULTIPLY_ALPHA_WEBGL) { this.__premul = v ? 1 : 0; g(this, X_UNPACK, this.__flipY, this.__premul); }
    else if (pn === C.UNPACK_COLORSPACE_CONVERSION_WEBGL) {}
    else g(this, O.PIXEL_STORE, pn, v);
  };
  GP.activeTexture = function(u){ g(this, O.ACTIVE_TEXTURE, u); };
  GP.bindTexture = function(t, tex){ g(this, O.BIND_TEXTURE, t, idOf(tex)); };
  GP.texParameteri = function(t, p, v){ g(this, O.TEX_PARAM_I, t, p, v); };
  GP.texParameterf = function(t, p, v){ g(this, O.TEX_PARAM_F, t, p, +v); };
  function isSource(x){ return x && typeof x === 'object' && (x.tagName === 'IMG' || x.tagName === 'CANVAS' || x.tagName === 'VIDEO'); }
  GP.texImage2D = function(t, level, ifmt, a, b, c, d, e, f){
    if (arguments.length === 6) {                    /* (target, level, internalformat, format, type, source) */
      var src = c && c.canvas && c.data ? null : c;
      if (c && c.data && c.width) { g(this, O.TEX_IMAGE, t, level, ifmt, c.width, c.height, a, b, bytesOf(this.__cv, c.data)); return; }   /* ImageData */
      if (c && c.close && c.width && !isSource(c) && c.__src) src = c.__src;   /* ImageBitmap */
      if (isSource(src)) g(this, X_TEX_EL, t, level, ifmt, a, b, src);
      return;
    }
    g(this, O.TEX_IMAGE, t, level, ifmt, a, b, d, e, bytesOf(this.__cv, f));   /* (w, h, border, format, type, pixels) */
  };
  GP.texSubImage2D = function(t, level, x, y, a, b, c, d, e){
    if (arguments.length === 7) {
      if (c && c.data && c.width) { g(this, O.TEX_SUB_IMAGE, t, level, x, y, c.width, c.height, a, b, bytesOf(this.__cv, c.data)); return; }
      if (isSource(c)) g(this, X_TEX_SUB_EL, t, level, x, y, a, b, c);
      return;
    }
    g(this, O.TEX_SUB_IMAGE, t, level, x, y, a, b, c, d, bytesOf(this.__cv, e));
  };
  GP.copyTexImage2D = function(t, l, f, x, y, w, h){ g(this, O.COPY_TEX_IMAGE, t, l, f, x, y, w, h); };
  GP.copyTexSubImage2D = function(t, l, xo, yo, x, y, w, h){ g(this, O.COPY_TEX_SUB_IMAGE, t, l, xo, yo, x, y, w, h); };
  GP.compressedTexImage2D = GP.compressedTexSubImage2D = function(){ this.__err = C.INVALID_ENUM; };
  GP.generateMipmap = function(t){ g(this, O.GEN_MIPMAP, t); };
  GP.bindFramebuffer = function(t, fb){ g(this, O.BIND_FRAMEBUFFER, t, idOf(fb)); };
  GP.framebufferTexture2D = function(t, a, tt, tex, l){ g(this, O.FB_TEXTURE, t, a, tt, idOf(tex), l); };
  GP.framebufferRenderbuffer = function(t, a, rt, rb){ g(this, O.FB_RENDERBUFFER, t, a, rt, idOf(rb)); };
  GP.checkFramebufferStatus = function(t){ return g(this, O.CHECK_FB, t); };
  GP.bindRenderbuffer = function(t, rb){ g(this, O.BIND_RENDERBUFFER, t, idOf(rb)); };
  GP.renderbufferStorage = function(t, f, w, h){ g(this, O.RB_STORAGE, t, f === C.DEPTH_STENCIL ? 0x88F0 : f, w, h); };   /* (DEPTH24_STENCIL8) */
  GP.readPixels = function(x, y, w, h, fmt, type, out){
    var s = g(this, O.READ_PIXELS, x, y, w, h, fmt, type), bin = atob(s || ''), u = new Uint8Array(out.buffer, out.byteOffset, out.byteLength);
    for (var i = 0, n = Math.min(bin.length, u.length); i < n; i++) u[i] = bin.charCodeAt(i);
  };
  GP.getError = function(){ if (this.__err) { var e = this.__err; this.__err = 0; return e; } return g(this, O.GET_ERROR); };
  var STR = {}; STR[C.VENDOR] = 1; STR[C.RENDERER] = 1; STR[C.VERSION] = 1; STR[C.SHADING_LANGUAGE_VERSION] = 1;
  GP.getParameter = function(pn){
    if (pn === C.VERSION) return 'WebGL 1.0 (OpenGL ES 2.0, Banana OS)';
    if (pn === C.SHADING_LANGUAGE_VERSION) return 'WebGL GLSL ES 1.0 (OpenGL ES GLSL ES 1.0)';
    if (pn === C.VENDOR) return 'Banana OS';
    if (pn === C.RENDERER) return 'WebGL (' + (g(this, O.GET_STRING, C.RENDERER) || 'Mesa') + ')';
    if (pn === C.CURRENT_PROGRAM) return this.__prog || null;
    if (pn === C.UNPACK_FLIP_Y_WEBGL) return !!this.__flipY;
    if (pn === C.UNPACK_PREMULTIPLY_ALPHA_WEBGL) return !!this.__premul;
    var v = g(this, O.GET_PARAM, pn) || [0];
    if (pn === C.VIEWPORT || pn === C.SCISSOR_BOX || pn === C.MAX_VIEWPORT_DIMS) return new Int32Array(v.length > 1 ? v : [v[0], v[0], 0, 0].slice(0, pn === C.MAX_VIEWPORT_DIMS ? 2 : 4));
    if (pn === C.COLOR_CLEAR_VALUE || pn === C.BLEND_COLOR) { var a = new Float32Array(4); for (var i = 0; i < 4; i++) a[i] = v[i] || 0; return a; }
    if (pn === C.DEPTH_RANGE || pn === C.ALIASED_LINE_WIDTH_RANGE || pn === C.ALIASED_POINT_SIZE_RANGE) return new Float32Array([v[0] || 0, v[1] || v[0] || 1]);
    if (pn === C.COLOR_WRITEMASK) return [!!v[0], v.length > 1 ? !!v[1] : true, v.length > 2 ? !!v[2] : true, v.length > 3 ? !!v[3] : true];
    if (pn === C.BLEND || pn === C.DEPTH_TEST || pn === C.CULL_FACE || pn === C.SCISSOR_TEST || pn === C.STENCIL_TEST || pn === C.DITHER || pn === C.DEPTH_WRITEMASK) return !!v[0];
    if (pn === C.COMPRESSED_TEXTURE_FORMATS) return new Uint32Array(0);
    return v[0];
  };
  GP.getBufferParameter = GP.getRenderbufferParameter = GP.getTexParameter = GP.getVertexAttrib = GP.getFramebufferAttachmentParameter = function(){ return null; };
  GP.getVertexAttribOffset = function(){ return 0; };
  GP.getUniform = function(){ return null; };
  GP.getSupportedExtensions = function(){ return ['OES_element_index_uint', 'OES_standard_derivatives']; };
  GP.getExtension = function(n){
    if (n === 'OES_element_index_uint' || n === 'OES_standard_derivatives') return { FRAGMENT_SHADER_DERIVATIVE_HINT_OES: 35723 };
    if (n === 'WEBGL_lose_context') { var gl = this; return { loseContext: function(){}, restoreContext: function(){} }; }
    if (n === 'WEBGL_debug_renderer_info') return { UNMASKED_VENDOR_WEBGL: 37445, UNMASKED_RENDERER_WEBGL: 37446 };
    return null;
  };
  GP.getContextAttributes = function(){ var o = this.__opts || {}; return { alpha: o.alpha !== false, depth: o.depth !== false, stencil: !!o.stencil,
    antialias: false, premultipliedAlpha: o.premultipliedAlpha !== false, preserveDrawingBuffer: !!o.preserveDrawingBuffer,
    powerPreference: 'default', failIfMajorPerformanceCaveat: false, desynchronized: false }; };
  GP.isContextLost = function(){ return !!__gl(this.__cv, X_LOST); };
  GP.finish = function(){ g(this, O.FINISH); __gl(this.__cv, X_PRESENT); };
  GP.flush = function(){};
  GP.makeXRCompatible = function(){ return Promise.reject(new Error('no WebXR')); };
  W.WebGLRenderingContext = GL;
  for (var kc in C) GL[kc] = C[kc];
  W.WebGLBuffer = WebGLBuffer; W.WebGLTexture = WebGLTexture; W.WebGLProgram = WebGLProgram; W.WebGLShader = WebGLShader;
  W.WebGLFramebuffer = WebGLFramebuffer; W.WebGLRenderbuffer = WebGLRenderbuffer; W.WebGLUniformLocation = WebGLUniformLocation;
  W.WebGLActiveInfo = WebGLActiveInfo; W.WebGLShaderPrecisionFormat = WebGLShaderPrecisionFormat;

  /* WebGL 2 (OpenGL ES 3.0 in the GPU worker): WebGL 1's context and more */
  var C2 = {};
  ('READ_BUFFER=3074 UNPACK_ROW_LENGTH=3314 UNPACK_SKIP_ROWS=3315 UNPACK_SKIP_PIXELS=3316 PACK_ROW_LENGTH=3330 ' +
   'PACK_SKIP_ROWS=3331 PACK_SKIP_PIXELS=3332 COLOR=6144 DEPTH=6145 STENCIL=6146 RED=6403 RGB8=32849 RGBA8=32856 ' +
   'RGB10_A2=32857 TEXTURE_BINDING_3D=32874 UNPACK_SKIP_IMAGES=32877 UNPACK_IMAGE_HEIGHT=32878 TEXTURE_3D=32879 ' +
   'TEXTURE_WRAP_R=32882 MAX_3D_TEXTURE_SIZE=32883 UNSIGNED_INT_2_10_10_10_REV=33640 MAX_ELEMENTS_VERTICES=33000 ' +
   'MAX_ELEMENTS_INDICES=33001 TEXTURE_MIN_LOD=33082 TEXTURE_MAX_LOD=33083 TEXTURE_BASE_LEVEL=33084 ' +
   'TEXTURE_MAX_LEVEL=33085 MIN=32775 MAX=32776 DEPTH_COMPONENT24=33190 MAX_TEXTURE_LOD_BIAS=34045 ' +
   'TEXTURE_COMPARE_MODE=34892 TEXTURE_COMPARE_FUNC=34893 CURRENT_QUERY=34917 QUERY_RESULT=34918 ' +
   'QUERY_RESULT_AVAILABLE=34919 STREAM_READ=35041 STREAM_COPY=35042 STATIC_READ=35045 STATIC_COPY=35046 ' +
   'DYNAMIC_READ=35049 DYNAMIC_COPY=35050 MAX_DRAW_BUFFERS=34852 MAX_FRAGMENT_UNIFORM_COMPONENTS=35657 ' +
   'MAX_VERTEX_UNIFORM_COMPONENTS=35658 SAMPLER_3D=35679 SAMPLER_2D_SHADOW=35682 FRAGMENT_SHADER_DERIVATIVE_HINT=35723 ' +
   'PIXEL_PACK_BUFFER=35051 PIXEL_UNPACK_BUFFER=35052 PIXEL_PACK_BUFFER_BINDING=35053 PIXEL_UNPACK_BUFFER_BINDING=35055 ' +
   'FLOAT_MAT2x3=35685 FLOAT_MAT2x4=35686 FLOAT_MAT3x2=35687 FLOAT_MAT3x4=35688 FLOAT_MAT4x2=35689 FLOAT_MAT4x3=35690 ' +
   'SRGB=35904 SRGB8=35905 SRGB8_ALPHA8=35907 COMPARE_REF_TO_TEXTURE=34894 RGBA32F=34836 RGB32F=34837 RGBA16F=34842 ' +
   'RGB16F=34843 VERTEX_ATTRIB_ARRAY_INTEGER=35069 MAX_ARRAY_TEXTURE_LAYERS=35071 MIN_PROGRAM_TEXEL_OFFSET=35076 ' +
   'MAX_PROGRAM_TEXEL_OFFSET=35077 MAX_VARYING_COMPONENTS=35659 TEXTURE_2D_ARRAY=35866 TEXTURE_BINDING_2D_ARRAY=35869 ' +
   'R11F_G11F_B10F=35898 UNSIGNED_INT_10F_11F_11F_REV=35899 RGB9_E5=35901 UNSIGNED_INT_5_9_9_9_REV=35902 ' +
   'TRANSFORM_FEEDBACK_BUFFER_MODE=35967 MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS=35968 ' +
   'TRANSFORM_FEEDBACK_VARYINGS=35971 TRANSFORM_FEEDBACK_BUFFER_START=35972 TRANSFORM_FEEDBACK_BUFFER_SIZE=35973 ' +
   'TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN=35976 RASTERIZER_DISCARD=35977 ' +
   'MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS=35978 MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS=35979 ' +
   'INTERLEAVED_ATTRIBS=35980 SEPARATE_ATTRIBS=35981 TRANSFORM_FEEDBACK_BUFFER=35982 ' +
   'TRANSFORM_FEEDBACK_BUFFER_BINDING=35983 RGBA32UI=36208 RGB32UI=36209 RGBA16UI=36214 RGB16UI=36215 RGBA8UI=36220 ' +
   'RGB8UI=36221 RGBA32I=36226 RGB32I=36227 RGBA16I=36232 RGB16I=36233 RGBA8I=36238 RGB8I=36239 RED_INTEGER=36244 ' +
   'RGB_INTEGER=36248 RGBA_INTEGER=36249 SAMPLER_2D_ARRAY=36289 SAMPLER_2D_ARRAY_SHADOW=36292 SAMPLER_CUBE_SHADOW=36293 ' +
   'UNSIGNED_INT_VEC2=36294 UNSIGNED_INT_VEC3=36295 UNSIGNED_INT_VEC4=36296 INT_SAMPLER_2D=36298 INT_SAMPLER_3D=36299 ' +
   'INT_SAMPLER_CUBE=36300 INT_SAMPLER_2D_ARRAY=36303 UNSIGNED_INT_SAMPLER_2D=36306 UNSIGNED_INT_SAMPLER_3D=36307 ' +
   'UNSIGNED_INT_SAMPLER_CUBE=36308 UNSIGNED_INT_SAMPLER_2D_ARRAY=36311 DEPTH_COMPONENT32F=36012 DEPTH32F_STENCIL8=36013 ' +
   'FLOAT_32_UNSIGNED_INT_24_8_REV=36269 FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING=33296 ' +
   'FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE=33297 FRAMEBUFFER_ATTACHMENT_RED_SIZE=33298 ' +
   'FRAMEBUFFER_ATTACHMENT_GREEN_SIZE=33299 FRAMEBUFFER_ATTACHMENT_BLUE_SIZE=33300 ' +
   'FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE=33301 FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE=33302 ' +
   'FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE=33303 FRAMEBUFFER_DEFAULT=33304 UNSIGNED_INT_24_8=34042 DEPTH24_STENCIL8=35056 ' +
   'UNSIGNED_NORMALIZED=35863 DRAW_FRAMEBUFFER_BINDING=36006 READ_FRAMEBUFFER=36008 DRAW_FRAMEBUFFER=36009 ' +
   'READ_FRAMEBUFFER_BINDING=36010 RENDERBUFFER_SAMPLES=36011 FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER=36052 ' +
   'MAX_COLOR_ATTACHMENTS=36063 FRAMEBUFFER_INCOMPLETE_MULTISAMPLE=36182 MAX_SAMPLES=36183 HALF_FLOAT=5131 RG=33319 ' +
   'RG_INTEGER=33320 R8=33321 RG8=33323 R16F=33325 R32F=33326 RG16F=33327 RG32F=33328 R8I=33329 R8UI=33330 R16I=33331 ' +
   'R16UI=33332 R32I=33333 R32UI=33334 RG8I=33335 RG8UI=33336 RG16I=33337 RG16UI=33338 RG32I=33339 RG32UI=33340 ' +
   'VERTEX_ARRAY_BINDING=34229 R8_SNORM=36756 RG8_SNORM=36757 RGB8_SNORM=36758 RGBA8_SNORM=36759 SIGNED_NORMALIZED=36764 ' +
   'COPY_READ_BUFFER=36662 COPY_WRITE_BUFFER=36663 COPY_READ_BUFFER_BINDING=36662 COPY_WRITE_BUFFER_BINDING=36663 ' +
   'UNIFORM_BUFFER=35345 UNIFORM_BUFFER_BINDING=35368 UNIFORM_BUFFER_START=35369 UNIFORM_BUFFER_SIZE=35370 ' +
   'MAX_VERTEX_UNIFORM_BLOCKS=35371 MAX_FRAGMENT_UNIFORM_BLOCKS=35373 MAX_COMBINED_UNIFORM_BLOCKS=35374 ' +
   'MAX_UNIFORM_BUFFER_BINDINGS=35375 MAX_UNIFORM_BLOCK_SIZE=35376 MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS=35377 ' +
   'MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS=35379 UNIFORM_BUFFER_OFFSET_ALIGNMENT=35380 ACTIVE_UNIFORM_BLOCKS=35382 ' +
   'UNIFORM_TYPE=35383 UNIFORM_SIZE=35384 UNIFORM_BLOCK_INDEX=35386 UNIFORM_OFFSET=35387 UNIFORM_ARRAY_STRIDE=35388 ' +
   'UNIFORM_MATRIX_STRIDE=35389 UNIFORM_IS_ROW_MAJOR=35390 UNIFORM_BLOCK_BINDING=35391 UNIFORM_BLOCK_DATA_SIZE=35392 ' +
   'UNIFORM_BLOCK_ACTIVE_UNIFORMS=35394 UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES=35395 ' +
   'UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER=35396 UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER=35398 ' +
   'INVALID_INDEX=4294967295 MAX_VERTEX_OUTPUT_COMPONENTS=37154 MAX_FRAGMENT_INPUT_COMPONENTS=37157 ' +
   'MAX_SERVER_WAIT_TIMEOUT=37137 OBJECT_TYPE=37138 SYNC_CONDITION=37139 SYNC_STATUS=37140 SYNC_FLAGS=37141 ' +
   'SYNC_FENCE=37142 SYNC_GPU_COMMANDS_COMPLETE=37143 UNSIGNALED=37144 SIGNALED=37145 ALREADY_SIGNALED=37146 ' +
   'TIMEOUT_EXPIRED=37147 CONDITION_SATISFIED=37148 WAIT_FAILED=37149 SYNC_FLUSH_COMMANDS_BIT=1 ' +
   'VERTEX_ATTRIB_ARRAY_DIVISOR=35070 ANY_SAMPLES_PASSED=35887 ANY_SAMPLES_PASSED_CONSERVATIVE=36202 ' +
   'SAMPLER_BINDING=35097 RGB10_A2UI=36975 INT_2_10_10_10_REV=36255 TRANSFORM_FEEDBACK=36386 ' +
   'TRANSFORM_FEEDBACK_PAUSED=36387 TRANSFORM_FEEDBACK_ACTIVE=36388 TRANSFORM_FEEDBACK_BINDING=36389 ' +
   'TEXTURE_IMMUTABLE_FORMAT=37167 MAX_ELEMENT_INDEX=36203 TEXTURE_IMMUTABLE_LEVELS=33503 TIMEOUT_IGNORED=-1 ' +
   'MAX_CLIENT_WAIT_TIMEOUT_WEBGL=37447').split(' ').forEach(function(kv){
    var i = kv.indexOf('='); C2[kv.slice(0, i)] = +kv.slice(i + 1); });
  for (var db = 0; db < 16; db++) { C2['DRAW_BUFFER' + db] = 34853 + db; C2['COLOR_ATTACHMENT' + db] = 36064 + db; }
  function GL2(cv, opts){ GL.call(this, cv, opts); this.__v2 = true; }
  var G2 = GL2.prototype = Object.create(GP);
  G2.constructor = GL2;
  for (var k1 in C) GL2[k1] = C[k1];
  for (var k2 in C2) { GL2[k2] = C2[k2]; G2[k2] = C2[k2]; }
  function part(v, off, len){                        /* (WebGL 2's srcOffset / srcLength) */
    if (off === undefined || !v || typeof v.subarray !== 'function') return v;
    return len ? v.subarray(off, off + len) : v.subarray(off);
  }
  G2.createVertexArray = function(){ return make(this, 'vao'); };
  G2.deleteVertexArray = del('vao');
  G2.bindVertexArray = function(v){ g(this, O.BIND_VAO, idOf(v)); };
  G2.createQuery = function(){ return make(this, 'query'); };
  G2.deleteQuery = del('query');
  G2.beginQuery = function(t, q){ g(this, O.BEGIN_QUERY, t, idOf(q)); };
  G2.endQuery = function(t){ g(this, O.END_QUERY, t); };
  G2.getQuery = function(){ return null; };
  G2.getQueryParameter = function(q, pn){ var v = g(this, O.QUERY_RESULT, idOf(q), pn); return pn === C2.QUERY_RESULT_AVAILABLE ? !!v : v >>> 0; };
  G2.createSampler = function(){ return make(this, 'sampler'); };
  G2.deleteSampler = del('sampler');
  G2.bindSampler = function(u, s){ g(this, O.BIND_SAMPLER, u, idOf(s)); };
  G2.samplerParameteri = function(s, p, v){ g(this, O.SAMPLER_PARAM_I, idOf(s), p, v); };
  G2.samplerParameterf = function(s, p, v){ g(this, O.SAMPLER_PARAM_F, idOf(s), p, +v); };
  G2.getSamplerParameter = function(){ return null; };
  G2.createTransformFeedback = function(){ return make(this, 'tf'); };
  G2.deleteTransformFeedback = del('tf');
  G2.bindTransformFeedback = function(t, tf){ g(this, O.BIND_TF, t, idOf(tf)); };
  G2.beginTransformFeedback = function(m){ g(this, O.BEGIN_TF, m); };
  G2.endTransformFeedback = function(){ g(this, O.END_TF); };
  G2.pauseTransformFeedback = function(){ g(this, O.PAUSE_TF); };
  G2.resumeTransformFeedback = function(){ g(this, O.RESUME_TF); };
  G2.transformFeedbackVaryings = function(p, names, mode){ g.apply(null, [this, O.TF_VARYINGS, idOf(p), mode].concat(Array.prototype.map.call(names, String))); };
  G2.getTransformFeedbackVarying = function(){ return null; };
  G2.isVertexArray = G2.isQuery = G2.isSampler = G2.isTransformFeedback = function(o){ return !!(o && !o.__dead && o.__id); };
  G2.texImage2D = function(t, level, ifmt, w, h, border, fmt, type, src, off){
    if (arguments.length <= 6) return GP.texImage2D.apply(this, arguments);
    if (isSource(src)) { g(this, X_TEX_EL, t, level, ifmt, fmt, type, src); return; }
    if (src && src.data && src.width) { g(this, O.TEX_IMAGE, t, level, ifmt, src.width, src.height, fmt, type, bytesOf(this.__cv, src.data)); return; }
    g(this, O.TEX_IMAGE, t, level, ifmt, w, h, fmt, type, typeof src === 'number' ? null : bytesOf(this.__cv, part(src, off)));
  };
  G2.texSubImage2D = function(t, level, x, y, w, h, fmt, type, src, off){
    if (arguments.length <= 7) return GP.texSubImage2D.apply(this, arguments);
    if (isSource(src)) { g(this, X_TEX_SUB_EL, t, level, x, y, fmt, type, src); return; }
    g(this, O.TEX_SUB_IMAGE, t, level, x, y, w, h, fmt, type, typeof src === 'number' ? null : bytesOf(this.__cv, part(src, off)));
  };
  G2.texImage3D = function(t, level, ifmt, w, h, d, border, fmt, type, src, off){
    g(this, O.TEX_IMAGE3D, t, level, ifmt, w, h, d, fmt, type, src && typeof src === 'object' && !isSource(src) ? bytesOf(this.__cv, part(src, off)) : null);
  };
  G2.texSubImage3D = function(t, level, x, y, z, w, h, d, fmt, type, src, off){
    g(this, O.TEX_SUB_IMAGE3D, t, level, x, y, z, w, h, d, fmt, type, src && typeof src === 'object' && !isSource(src) ? bytesOf(this.__cv, part(src, off)) : null);
  };
  G2.texStorage2D = function(t, lv, f, w, h){ g(this, O.TEX_STORAGE2D, t, lv, f, w, h); };
  G2.texStorage3D = function(t, lv, f, w, h, d){ g(this, O.TEX_STORAGE3D, t, lv, f, w, h, d); };
  G2.compressedTexImage3D = G2.compressedTexSubImage3D = function(){ this.__err = C.INVALID_ENUM; };
  G2.copyTexSubImage3D = function(){ this.__err = C.INVALID_OPERATION; };
  G2.drawBuffers = function(list){ g.apply(null, [this, O.DRAW_BUFFERS].concat(Array.prototype.slice.call(list))); };
  G2.readBuffer = function(s){ g(this, O.READ_BUFFER, s); };
  G2.blitFramebuffer = function(a, b, c, d, e, f, h, i, mask, filter){ g(this, O.BLIT_FB, a, b, c, d, e, f, h, i, mask, filter); drew(this); };
  G2.renderbufferStorageMultisample = function(t, s, f, w, h){ g(this, O.RB_STORAGE_MS, t, s, f, w, h); };
  G2.framebufferTextureLayer = function(t, a, tex, lv, layer){ g(this, O.FB_TEX_LAYER, t, a, idOf(tex), lv, layer); };
  G2.invalidateFramebuffer = G2.invalidateSubFramebuffer = function(){};
  G2.getInternalformatParameter = function(t, f, pn){
    if (pn !== 32937) return null;                     /* SAMPLES */
    var max = this.getParameter(C2.MAX_SAMPLES) || 0, out = [];
    for (var s = max; s > 1; s >>= 1) out.push(s);
    return new Int32Array(out);
  };
  G2.bindBufferBase = function(t, i, b){ g(this, O.BIND_BUFFER_BASE, t, i, idOf(b)); };
  G2.bindBufferRange = function(t, i, b, off, size){ g(this, O.BIND_BUFFER_RANGE, t, i, idOf(b), off, size); };
  G2.getUniformBlockIndex = function(p, n){ return g(this, O.UNIFORM_BLOCK_INDEX, idOf(p), String(n)) >>> 0; };
  G2.uniformBlockBinding = function(p, i, b){ g(this, O.UNIFORM_BLOCK_BINDING, idOf(p), i, b); };
  G2.getActiveUniformBlockParameter = function(p, i, pn){
    if (pn === C2.UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES) return new Uint32Array(0);
    var v = g(this, O.UNIFORM_BLOCK_PARAM, idOf(p), i, pn);
    return pn === C2.UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER || pn === C2.UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER ? !!v : v;
  };
  G2.getActiveUniformBlockName = function(){ return null; };
  G2.getUniformIndices = function(p, names){ return null; };
  G2.getActiveUniforms = function(){ return null; };
  G2.getFragDataLocation = function(p, n){ return g(this, O.FRAG_DATA_LOC, idOf(p), String(n)); };
  function u32(cv, v){ return bytesOf(cv, v instanceof Uint32Array ? v : new Uint32Array(v)); }
  [1, 2, 3, 4].forEach(function(n){
    G2['uniform' + n + 'ui'] = function(l){ if (!l) return; var a = [this, O.UNIFORM_UI, loc(l), n]; for (var i = 1; i <= n; i++) a.push(arguments[i] >>> 0); g.apply(null, a); };
    G2['uniform' + n + 'uiv'] = function(l, v, off, len){ if (!l || !v) return; v = part(v, off, len); g(this, O.UNIFORM_UIV, loc(l), n, (v.length / n) | 0, u32(this.__cv, v)); };
    G2['uniform' + n + 'fv'] = function(l, v, off, len){ GP['uniform' + n + 'fv'].call(this, l, part(v, off, len)); };
    G2['uniform' + n + 'iv'] = function(l, v, off, len){ GP['uniform' + n + 'iv'].call(this, l, part(v, off, len)); };
    G2['vertexAttribI4' + (n === 1 ? 'i' : n === 2 ? 'ui' : n === 3 ? 'iv' : 'uiv')] = n < 3
      ? function(i, a, b, c, d){ g(this, O.ATTRIB_I, i, n === 2 ? 1 : 0, a, b, c, d); }
      : function(i, v){ g(this, O.ATTRIB_I, i, n === 4 ? 1 : 0, v[0], v[1], v[2], v[3]); };
  });
  [[2, 2], [3, 3], [4, 4], [2, 3], [2, 4], [3, 2], [3, 4], [4, 2], [4, 3]].forEach(function(cr){
    var name = 'uniformMatrix' + cr[0] + (cr[0] === cr[1] ? '' : 'x' + cr[1]) + 'fv';
    G2[name] = function(l, tr, v, off, len){
      if (!l || !v) return;
      v = part(v, off, len);
      g(this, O.UNIFORM_MAT_NS, loc(l), cr[0], cr[1], (v.length / (cr[0] * cr[1])) | 0, f32(this.__cv, v));
    };
  });
  G2.vertexAttribIPointer = function(i, size, type, stride, off){ g(this, O.ATTRIB_IPTR, i, size, type, stride, off); };
  G2.vertexAttribDivisor = function(i, d){ g(this, O.ATTRIB_DIVISOR, i, d); };
  G2.drawArraysInstanced = function(m, first, count, n){ g(this, O.DRAW_ARRAYS_INST, m, first, count, n); drew(this); };
  G2.drawElementsInstanced = function(m, count, type, off, n){ g(this, O.DRAW_ELEMENTS_INST, m, count, type, off, n); drew(this); };
  G2.drawRangeElements = function(m, s, e, count, type, off){ g(this, O.DRAW_RANGE_ELEMENTS, m, s, e, count, type, off); drew(this); };
  G2.clearBufferfv = function(b, d, v, off){ v = part(v, off); g(this, O.CLEAR_BUFFER_FV, b, d, +v[0] || 0, +v[1] || 0, +v[2] || 0, +v[3] || 0); drew(this); };
  G2.clearBufferiv = function(b, d, v, off){ v = part(v, off); g(this, O.CLEAR_BUFFER_IV, 0, b, d, v[0] | 0, v[1] | 0, v[2] | 0, v[3] | 0); drew(this); };
  G2.clearBufferuiv = function(b, d, v, off){ v = part(v, off); g(this, O.CLEAR_BUFFER_IV, 1, b, d, v[0] >>> 0, v[1] >>> 0, v[2] >>> 0, v[3] >>> 0); drew(this); };
  G2.clearBufferfi = function(b, d, depth, st){ g(this, O.CLEAR_BUFFER_FI, b, d, +depth, st | 0); drew(this); };
  G2.copyBufferSubData = function(rt, wt, ro, wo, size){ g(this, O.COPY_BUFFER_SUB, rt, wt, ro, wo, size); };
  G2.getBufferSubData = function(t, off, dst, dstOff, len){
    var u = new Uint8Array(dst.buffer, dst.byteOffset, dst.byteLength), es = dst.BYTES_PER_ELEMENT || 1;
    var start = (dstOff || 0) * es, n = len ? len * es : u.length - start;
    var s = g(this, O.GET_BUFFER_SUB, t, off, n), bin = atob(s || '');
    for (var i = 0; i < bin.length && start + i < u.length; i++) u[start + i] = bin.charCodeAt(i);
  };
  G2.bufferData = function(t, d, usage, off, len){
    if (off !== undefined && d && typeof d.subarray === 'function') d = part(d, off, len);
    GP.bufferData.call(this, t, d, usage);
  };
  G2.bufferSubData = function(t, dstOff, d, off, len){ GP.bufferSubData.call(this, t, dstOff, off !== undefined ? part(d, off, len) : d); };
  G2.fenceSync = function(){ var s = new WebGLSync(); s.__sync = true; return s; };
  G2.isSync = function(s){ return !!(s && s.__sync); };
  G2.deleteSync = G2.waitSync = function(){};
  G2.clientWaitSync = function(){ return C2.ALREADY_SIGNALED; };
  G2.getSyncParameter = function(s, pn){
    return pn === C2.SYNC_STATUS ? C2.SIGNALED : pn === C2.OBJECT_TYPE ? C2.SYNC_FENCE : pn === C2.SYNC_CONDITION ? C2.SYNC_GPU_COMMANDS_COMPLETE : 0;
  };
  G2.getIndexedParameter = function(){ return null; };
  G2.getParameter = function(pn){
    if (pn === C.VERSION) return 'WebGL 2.0 (OpenGL ES 3.0, Banana OS)';
    if (pn === C.SHADING_LANGUAGE_VERSION) return 'WebGL GLSL ES 3.00 (OpenGL ES GLSL ES 3.0)';
    if (pn === C2.VERTEX_ARRAY_BINDING || pn === C2.SAMPLER_BINDING || pn === C2.UNIFORM_BUFFER_BINDING ||
        pn === C2.TRANSFORM_FEEDBACK_BINDING || pn === C2.READ_FRAMEBUFFER_BINDING) return null;
    return GP.getParameter.call(this, pn);
  };
  G2.getSupportedExtensions = function(){ return ['EXT_color_buffer_float', 'OES_texture_float_linear', 'WEBGL_debug_renderer_info']; };
  G2.getExtension = function(n){
    if (n === 'EXT_color_buffer_float' || n === 'OES_texture_float_linear' || n === 'EXT_color_buffer_half_float') return {};
    if (n === 'OES_element_index_uint' || n === 'OES_standard_derivatives') return null;   /* (core in WebGL 2) */
    return GP.getExtension.call(this, n);
  };
  W.WebGL2RenderingContext = GL2;
  W.WebGLVertexArrayObject = WebGLVertexArrayObject; W.WebGLQuery = WebGLQuery; W.WebGLSampler = WebGLSampler;
  W.WebGLTransformFeedback = WebGLTransformFeedback; W.WebGLSync = WebGLSync;
  /* getContext('webgl' / 'webgl2') (the 2D part is in the canvas block) */
  W.__webglContext = function(cv, type, opts){
    var two = type === 'webgl2';
    if (cv.__gl) return !!cv.__gl.__v2 === two ? cv.__gl : null;
    if (cv.__ctx2d) return null;                     /* (one kind of context a canvas) */
    opts = opts || {};
    var v = __gl(cv, X_OPEN, opts.premultipliedAlpha === false ? 0 : 1, two ? 3 : 2);
    if (!v || (two && v !== 3)) return null;         /* (no OpenGL ES 3.0 here: no WebGL 2) */
    return cv.__gl = two ? new GL2(cv, opts) : new GL(cv, opts);
  };
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
