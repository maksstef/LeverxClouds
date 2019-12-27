/*
	This is the inner core of cds: its type system, bootstrapped from in-place
	CSN models itself. Besides the actual root types, a set of common and
	recommended scalar types is added to builtin.types.
*/
const classes = require ('./classes')

// Type system roots -> can be used with instanceof
const roots = classes.bootstrap ({
	context: {},
	type: {},
		scalar: {type:'type'},
			string: {type:'scalar'},
			number: {type:'scalar'},
			boolean: {type:'scalar'},
			date: {type:'scalar'},
		struct: {type:'type'},
		array: {type:'type'},
	entity: {type:'struct'},
	Association: {type:'type'},
	Composition: {type:'Association'},
	service: {type:'context'},
})

// Common and recommended types
const common = _linked ({ __proto__: roots,
	UUID: { type:'string', length:36 },
	Boolean: {type:'boolean'},
	Integer: {type:'number'},
		Integer16: {type:'Integer'},
		Integer32: {type:'Integer'},
		Integer64: {type:'Integer'},
	Decimal: {type:'number'},
	DecimalFloat: {type:'number'},
	Float: {type:'number'},
	Double: {type:'number'},
	DateTime: {type:'date'},
	Date: {type:'date'},
	Time: {type:'date'},
	Timestamp: {type:'date'},
	String: {type:'string'},
	Binary: {type:'string'},
	LargeString: {type:'string'},
	LargeBinary: {type:'string'},
})

// Links definitions, essentially by: def.__proto__ = definitions[def.type]
function _linked (defs) {
	for (let n of Object.keys(defs)) {
		const {type} = defs[n];  defs[n] = { type, __proto__:defs[type] }
	}
	return defs
}

// Puts all common types into a namespace 'cds'
function _cds (common) {
	const names = [...Object.keys(common), 'Association', 'Composition']
	const cds = {__proto__:common}; for (let n of names)  cds['cds.'+n] = cds[n]
	return cds
}

// returns builtin { types, classes }
module.exports = { types:_cds(common), classes }
