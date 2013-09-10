
database: a.db
id: id

input {
    output table: hhs
    overwrite: no
}

include: cheks

fields{
    QRACE1: cat 200,100,F11,500,410,W02,420,CHECK,MISS
    QRACE2: cat null,110,105,CHECK,MISS
    CENRACE1: int 0-6
    CENRACE2: int 0, 7-21
}

