#ifndef GBI_H
#define GBI_H

// F3DEX2 / RDP command opcodes and state bits used by the display-list interpreter.

// RSP (F3DEX2)
#define G_NOOP              0x00
#define G_VTX               0x01
#define G_MODIFYVTX         0x02
#define G_CULLDL            0x03
#define G_BRANCH_Z          0x04
#define G_TRI1              0x05
#define G_TRI2              0x06
#define G_QUAD              0x07
#define G_LINE3D            0x08 // L3DEX2
#define G_DMA_IO            0xD6
#define G_TEXTURE           0xD7
#define G_POPMTX            0xD8
#define G_GEOMETRYMODE      0xD9
#define G_MTX               0xDA
#define G_MOVEWORD          0xDB
#define G_MOVEMEM           0xDC
#define G_LOAD_UCODE        0xDD
#define G_DL                0xDE
#define G_ENDDL             0xDF
#define G_SPNOOP            0xE0
#define G_RDPHALF_1         0xE1
#define G_SETOTHERMODE_L    0xE2
#define G_SETOTHERMODE_H    0xE3

// RDP
#define G_TEXRECT           0xE4
#define G_TEXRECTFLIP       0xE5
#define G_RDPLOADSYNC       0xE6
#define G_RDPPIPESYNC       0xE7
#define G_RDPTILESYNC       0xE8
#define G_RDPFULLSYNC       0xE9
#define G_SETKEYGB          0xEA
#define G_SETKEYR           0xEB
#define G_SETCONVERT        0xEC
#define G_SETSCISSOR        0xED
#define G_SETPRIMDEPTH      0xEE
#define G_RDPSETOTHERMODE   0xEF
#define G_LOADTLUT          0xF0
#define G_RDPHALF_2         0xF1
#define G_SETTILESIZE       0xF2
#define G_LOADBLOCK         0xF3
#define G_LOADTILE          0xF4
#define G_SETTILE           0xF5
#define G_FILLRECT          0xF6
#define G_SETFILLCOLOR      0xF7
#define G_SETFOGCOLOR       0xF8
#define G_SETBLENDCOLOR     0xF9
#define G_SETPRIMCOLOR      0xFA
#define G_SETENVCOLOR       0xFB
#define G_SETCOMBINE        0xFC
#define G_SETTIMG           0xFD
#define G_SETZIMG           0xFE
#define G_SETCIMG           0xFF

// G_MTX parameters (F3DEX2 XORs the push bit)
#define G_MTX_PUSH          0x01
#define G_MTX_LOAD          0x02
#define G_MTX_PROJECTION    0x04

// G_MOVEWORD indices
#define G_MW_MATRIX         0x00
#define G_MW_NUMLIGHT       0x02
#define G_MW_CLIP           0x04
#define G_MW_SEGMENT        0x06
#define G_MW_FOG            0x08
#define G_MW_LIGHTCOL       0x0A
#define G_MW_PERSPNORM      0x0E

// G_MOVEMEM indices
#define G_MV_VIEWPORT       0x08
#define G_MV_LIGHT          0x0A
#define G_MV_MATRIX         0x0E

// Geometry mode (F3DEX2)
#define G_ZBUFFER               0x00000001
#define G_SHADE                 0x00000004
#define G_CULL_FRONT            0x00000200
#define G_CULL_BACK             0x00000400
#define G_CULL_BOTH             0x00000600
#define G_FOG                   0x00010000
#define G_LIGHTING              0x00020000
#define G_TEXTURE_GEN           0x00040000
#define G_TEXTURE_GEN_LINEAR    0x00080000
#define G_SHADING_SMOOTH        0x00200000

// Other mode L
#define G_AC_MASK           0x00000003
#define G_AC_THRESHOLD      0x00000001
#define Z_CMP               0x00000010
#define Z_UPD               0x00000020
#define ZMODE_MASK          0x00000C00
#define ZMODE_DEC           0x00000C00
#define CVG_X_ALPHA         0x00001000
#define G_BL_A_MEM          1
#define G_BL_CLR_FOG        3

// Other mode H
#define G_CYC_SHIFT         20
#define G_CYC_1CYCLE        0
#define G_CYC_2CYCLE        1
#define G_CYC_COPY          2
#define G_CYC_FILL          3
#define G_TF_SHIFT          12
#define G_TF_POINT          0
#define G_TT_SHIFT          14
#define G_TT_IA16           3

// Image formats / sizes
#define G_IM_FMT_RGBA       0
#define G_IM_FMT_YUV        1
#define G_IM_FMT_CI         2
#define G_IM_FMT_IA         3
#define G_IM_FMT_I          4
#define G_IM_SIZ_4b         0
#define G_IM_SIZ_8b         1
#define G_IM_SIZ_16b        2
#define G_IM_SIZ_32b        3

// Tile clamp/mirror bits
#define G_TX_MIRROR         0x1
#define G_TX_CLAMP          0x2

#endif
