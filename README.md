# demex-canopen

This repository contains a simple example of using
features of canopen-binding

It defines the API "demexco".

The binding is intended to run in the same binder than the canopen binding
and it leverages the 'get' verb of canopen.

It translates JSON requests to binary request and returned binary values
to JSON values.

## Verbs of 'demexco'

The API 'demexco' defines the following verbs:

- info: get some info about the binding
- get: get values described in the request
- add: add and subscribe to a periodic update of values
- rem: remove and unsubscribe a periodic event

### Verb 'info'

That verb is not taking any parameter.

It returns a json string describing the binding.

### Verb 'get'

This verb accept a JSON description of the required values (see below).

It returns an array of values corresponding to the required request.

Example:

```
{ "itf": 0, "id": 4, "reg": "1400", "subreg": 1, "type": "i16", "tpdo": false }
```

could return 

```
[45]
```

### Verb 'add'

This verb accept a JSON description of the required values, the event name and the period.

The JSON must be an object having the below fields:

- name: a string naming the event (the produced event will be "demexco/name")
- periodms: a number being the period in milliseconds
- get: the description of the requested values as in verb "get" (see below).

It returns a status.

Example:

```
{"name":"myevt","periodms":5000,"get":{ "itf": 0, "id": 4, "reg": "1400", "subreg": 1, "type": "i16", "tpdo": false }}
```

### Verb 'rem'

This verb accept a JSON of the event name to remove.

It returns a status.

Example:

```
{"name":"myevt"}
```

but could also be

```
"myevt"
```


## Description of required values and corresponding results

The verbs 'get' and 'add' are taking a JSON description of the required values.

Basically, that description is an array of structures describing the expected values
as on the below example:

```
[
  { "itf": 0, "id": 4, "reg": "1400", "subreg": 1, "type": "i16", "tpdo": false },
  { "itf": 0, "id": 4, "reg": "1400", "subreg": 2, "type": "i16", "tpdo": false },
  { "itf": 0, "id": 4, "reg": "1400", "subreg": 3, "type": "i16", "tpdo": false },
  { "itf": 0, "id": 4, "reg": "1f00", "subreg": 1, "type": "u32", "tpdo": false },
  { "itf": 0, "id": 4, "reg": "1f00", "subreg": 2, "type": "u32", "tpdo": false },
  { "itf": 0, "id": 4, "reg": "1f00", "subreg": 3, "type": "u32", "tpdo": false }
]
```

Where:

- itf: index of the CAN interface
- id: id of the slave (or 0 for self SDO)
- reg: number of the register
- subreg: number of the subregister
- type: type of the value, can be 'u8', 'i8', 'u16', 'i16', 'u32', 'i32', 'u64', 'i64'
- tpdo: a boolean true if value is TPDO (optionnal, default is false)

Numeric values can be given in HEXA but must then be encoded as strings.
For example, 32769 can also be encoded by "8001" or "0x8001".

But it is possible to factorize the items. The above example can also be written:

```
  {
     "itf": 0,
     "id": 4, 
     "items": [
        {
	   "reg": "1400",
           "type": "i16",
           "items": [
               "subreg": 1,
               "subreg": 2,
               "subreg": 3
	   ]
	},
        {
	   "reg": "1f00",
           "type": "u32",
           "items": [
               "subreg": 1,
               "subreg": 2,
               "subreg": 3
	   ]
	}
     ]
}
```

The produced values are given in the encountered definition order.

In the above examples, the order is the same.

An example of returned values for such request is:

```
[1,2,3,78,45,96]
```

