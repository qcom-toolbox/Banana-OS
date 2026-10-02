// newer JavaScript: compared with Node.js output
const log = (...a) => console.log(a.map(x => typeof x === "object" ? JSON.stringify(x) : String(x)).join(" "));

// regex
log("a1b22c333".match(/\d+/g));
log("2026-10-02".replace(/(\d+)-(\d+)-(\d+)/, "$3/$2/$1"));
log("Hello World".replace(/o/g, m => m.toUpperCase()));
log("a, b ,c".split(/\s*,\s*/));
log(/^[\w.]+@\w+\.\w+$/.test("bob.s@example.com"), /^\d+$/.test("12a"));
const m = /(?<y>\d{4})-(?<m>\d\d)/.exec("on 2026-10");
log(m[0], m.index, m.groups.y, m.groups.m);
log("x".padStart(3, "-"), "camelCaseText".replace(/([A-Z])/g, " $1").toLowerCase());
const re = /a/g; let n = 0; while (re.exec("banana")) n++; log("count", n);
log("aaa".search(/b/), "abc".search(/c/));
log([..."a1b2".matchAll(/[a-z](\d)/g)].map(x => x[1]));

// spread, rest, destructuring
const sum = (...xs) => xs.reduce((a, b) => a + b, 0);
log(sum(...[1, 2, 3], 4));
const [p, , q = 9, ...rest] = [1, 2, undefined, 4, 5];
log(p, q, rest);
const { a, b: { c }, d = "def", ...others } = { a: 1, b: { c: 2 }, e: 5, f: 6 };
log(a, c, d, others);
let x1 = 1, y1 = 2; [x1, y1] = [y1, x1]; log(x1, y1);
log({ ...{ k: 1 }, j: 2 }, [...new Set([1, 1, 2, 3])]);
function f({ name, age = 30 } = {}) { return name + ":" + age; }
log(f({ name: "ann" }), f());
for (const [k, v] of Object.entries({ u: 1, w: 2 })) log(k, v);

// classes
class Animal {
    #secret = 42;
    static count = 0;
    constructor(name) { this.name = name; Animal.count++; }
    speak() { return this.name + " makes a sound"; }
    get upper() { return this.name.toUpperCase(); }
    set nick(v) { this._nick = "~" + v; }
    get secret() { return this.#secret; }
    static create(n) { return new this(n); }
}
class Dog extends Animal {
    tricks = [];
    constructor(name) { super(name); this.kind = "dog"; }
    speak() { return super.speak() + " (woof)"; }
}
const dog = new Dog("rex");
dog.nick = "R";
log(dog.speak(), dog.upper, dog._nick, dog.secret, dog.kind, dog.tricks.length);
log(dog instanceof Dog, dog instanceof Animal, Animal.count, Animal.create("cat").speak());
class E2 extends Error { constructor(m) { super(m); this.name = "E2"; } }
try { throw new E2("boom"); } catch (e) { log(e.name, e.message, e instanceof Error); }

// Map / Set
const mp = new Map([["a", 1]]); mp.set("b", 2).set("a", 3);
log(mp.size, mp.get("a"), mp.has("z"), [...mp.keys()], [...mp.entries()]);
const st = new Set("hello"); log(st.size, [...st].join(""));

// operators & misc
log("x" in { x: 1 }, 5 >>> 1, -1 >>> 28, 2 ** 10);
let z = null; z ??= 5; let w = 0; w ||= 7; let t = 1; t &&= 9; log(z, w, t);
const o2 = { deep: null }; log(o2.deep?.x, o2.fn?.(), o2.arr?.[0]);
outer: for (let i = 0; i < 3; i++) { for (let j = 0; j < 3; j++) { if (j === 1) continue outer; if (i === 2) break outer; log("ij", i, j); } }
log([1, [2, [3, [4]]]].flat(2), [1, 2].flatMap(v => [v, v * 10]), Array.from({ length: 3 }, (_, i) => i * i));
log(Object.fromEntries([["k", 1]]), Number.isInteger(5.0), Math.hypot(3, 4), Math.trunc(-4.7));
const bound = function (g) { return this.v + g; }.bind({ v: 10 }); log(bound(5));
log(typeof Symbol.iterator, [3, 1, 2].findLast(v => v > 1));

// promises & async
const later = v => new Promise(res => res(v));
async function go() {
    const r = await later(21);
    const all = await Promise.all([later(1), 2, later(3)]);
    try { await Promise.reject(new Error("nope")); } catch (e) { log("caught", e.message); }
    return r * 2 + all.length;
}
go().then(v => log("async result", v));
Promise.resolve(1).then(v => v + 1).then(v => log("chain", v)).finally(() => log("finally"));
log("sync end");
