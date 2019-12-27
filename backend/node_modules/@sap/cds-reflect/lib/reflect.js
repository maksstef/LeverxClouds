const { Association, entity, any } = require('./core').classes
const _reflected = Symbol(); any.prototype[_reflected] = true
const _class = Symbol(); any[_class] = true

const cds_reflect = cached ((x,_link) => (
	is.Model(x) ? new ReflectedModel (x,_link) :
	is.Association(x) ? new Association (x) :
	is.entity(x) ? new entity (x) :
	new any (x)
), _reflected)

class ReflectedModel extends any {

	constructor (csn, _link) {
		super(csn) .set ('_parsed',csn)
		if (!_link)  this.forall ((d,name,parent) => {
			d.kind || set (d,'kind', parent ? 'element' : d.query ? 'entity' : 'type')
			d.name || set (d,'name', name)
		})
	}

	*each (x, defs=this.definitions, pick=is(x)) {
		for (let d in defs) if (pick(defs[d])) yield defs[d]
	}

	find (x, defs=this.definitions) {
		for (let d of this.each(x,defs))  return d //> first match
	}

	all (x, defs=this.definitions) {
		return [...this.each(x,defs)] //> materialize as array
	}

	foreach (x, visit, defs=this.definitions) {
		const _pick = visit ? is(x) : (visit=x) && is.any
		for (let d in defs) if (_pick(defs[d]))  visit (defs[d],d, undefined, defs)
		return this
	}

	forall (x, visit, defs=this.definitions) {
		const _pick = visit ? is(x) : (visit=x) && is.any
		_recurse (defs)
		function _recurse (defs, parent) {
			for (let each in defs) {
				const d = defs[each]
				if (_pick(d))  visit (d, each, parent, defs)
				_recurse (own(d,'elements'), d)
				_inline (own(d,'target'))
				_inline (own(d,'via'))
			}
		}
		function _inline(d) {
			if (typeof d === 'object')  _recurse (d.elements,d)
		}
		return this
	}

	childrenOf (x, filter=is.any, property) {
		let ns = typeof x === 'string' ? x : x.namespace || x.name || '';  if (ns)  ns += '.'
		const children = (ns) => ns ? this.childrenOf (ns,filter) : children
		const defs=this.definitions
		for (let fqn in defs) if (fqn.startsWith(ns)) {
			let d = defs[fqn], rn = fqn.slice(ns.length)
			if (filter(d,rn))  children[rn] = defs[fqn]
		}
		if (property)  set (x, property, children)
		return children
	}

	get exports() { return this.childrenOf (this, (_,rn)=>!rn.includes('.'), 'exports') }
	get entities() { return this.childrenOf (this, is.entity, 'entities') }
	get services() { return this.childrenOf (this, is.service, 'services') }
}

const is = Object.assign (x => {
	if (!x) throw new Error ('missing filter for model reflection: '+ x)
	if (x[_class])  return  d => d instanceof x || d.kind == x.name
	if (typeof x === 'function')  return x
	if (typeof x === 'string')  return  is[x] || (d => d.kind === x)
	throw new Error ('invalid filter for model reflection: '+ x)
},{
	Model: d => d.definitions !== undefined || d.extensions !== undefined || d.version !== undefined,
	struct: d => d.elements !== undefined  ||  d.query !== undefined,
	entity: d => d.kind === 'entity'  ||  d.kind === 'view'  ||  d.query !== undefined,
	view: d => d.query !== undefined,
	service: d => d.kind === 'service',
	Association: d => d.type === 'cds.Association' || d.type === 'cds.Composition',
	Composition: d => d.type === 'cds.Composition',
	any: ()=>true,
})

function own (d,p) {
	const pd = Reflect.getOwnPropertyDescriptor(d,p)
	return pd ? pd.value || pd.get(d) : false
}
function set (o,p,value) {
	return Object.defineProperty (o,p,{value,configurable:1,writable:1})
}

function cached (fn, tag=Symbol()) {
	const cache = new WeakMap
	return (x,...etc) => {
		if (x[tag]) return x
		let cached = cache.get(x)
		if (!cached)  cache.set(x, cached = Object.defineProperty (fn(x,...etc), tag, {value:1}))
		return cached
	}
}

module.exports = Object.assign (cds_reflect, { cached, own, set })
