// numbers, strings, operators
console.log(1 + 2 * 3, 10 / 4, 7 % 3, 2 ** 10, 0.1 + 0.2, -5 % 3);
console.log("a" + 1 + 2, 1 + 2 + "a", "5" * "2", typeof "x", typeof 1, typeof null, typeof undefined, typeof {}, typeof function(){});
console.log(1 == "1", 1 === "1", null == undefined, null === undefined, NaN == NaN, [1,2] + "");
let s = "Hello, World";
console.log(s.length, s.toUpperCase(), s.indexOf("World"), s.slice(-5), s.split(", "), s.replace("World", "Banana"));
console.log(`template ${s.length * 2} ok`, "pad".padStart(6, "*"), " trim ".trim() + "|");
// arrays
const a = [5, 3, 8, 1];
a.push(9);
console.log(a.length, a.join("-"), a.map(x => x * 2), a.filter(x => x > 4), a.reduce((s, x) => s + x, 0));
console.log(a.slice().sort((x, y) => x - y), a.indexOf(8), a.includes(42));
// objects
const o = { name: "Banana", ver: 0.5, tags: ["os", "fun"], greet() { return "hi " + this.name; } };
o.extra = true;
console.log(o.name, o["ver"], o.tags[1], o.greet(), Object.keys(o).join(","), JSON.stringify(o));
console.log(JSON.parse('{"a":[1,2,{"b":null}],"c":"d"}').a[2].b, JSON.stringify([1,"two",true,null]));
// control flow, functions, closures
function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
let out = [];
for (let i = 0; i < 10; i++) { if (i % 2) continue; out.push(fib(i)); }
console.log(out.join(","));
const counters = [];
for (let i = 0; i < 3; i++) counters.push(() => i);
console.log(counters.map(f => f()).join(","));
function makeAdder(n) { return function(x) { return x + n; }; }
console.log(makeAdder(10)(5));
let k = 0; while (true) { k++; if (k > 4) break; }
do { k--; } while (k > 2);
console.log(k);
switch (k) { case 1: console.log("one"); break; case 2: console.log("two"); default: console.log("fallthrough"); }
try { null.x; } catch (e) { console.log("caught", e.message); } finally { console.log("finally"); }
try { throw new Error("boom"); } catch (e) { console.log(e.name + ": " + e.message); }
for (const ch of "abc") console.log(ch);
for (const key in {x: 1, y: 2}) console.log(key);
console.log(Math.max(3, 9, 2), Math.floor(4.7), Math.round(4.5), Math.sqrt(16), Math.abs(-3), Math.pow(2, 0.5).toFixed(4));
console.log((3.14159).toFixed(2), (255).toString(16), parseInt("42px"), parseFloat("3.5e2"), Number("12"), isNaN("x"));
console.log(String(123) + 1, [1, [2, 3]].toString(), [3, 1, 2].sort().join(""));
function Point(x, y) { this.x = x; this.y = y; }
Point.prototype.len = function() { return Math.sqrt(this.x * this.x + this.y * this.y); };
const p = new Point(3, 4);
console.log(p.len(), p.x);
const d = new Date();
console.log(typeof d.getFullYear(), d.getFullYear() > 2020);
let x = 5; x += 3; x *= 2; x -= 1; console.log(x, x++, ++x, x--);
console.log(null ?? "default", 0 || "or", 0 ?? "nn", undefined?.foo);
