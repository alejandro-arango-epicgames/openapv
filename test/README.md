# Materials for APV codec testing

## Test bitstream
"bitstream" folder has the encoded bitstreams for decoder conformance testing.

| No. | Bitstream Name | Description                                                  | Profile&nbsp;&nbsp; | Level | Band | Frame Rate | Resolution | # of Frame | MD5 sum of bitstream             |
|-----|----------------|--------------------------------------------------------------|---------------------|-------|------|------------|------------|------------|----------------------------------|
| 1   | tile_A         | one-tile per   one-picture                                   | 422-10              | 4.1   | 2    | 60 fps     | 3840x2160  | 3          | 74c5c0ca1bd2cfb28c6e2e0673e965f9 |
| 2   | tile_B         | Tile size = min size   tile (256x128)                        | 422-10              | 4.1   | 2    | 60 fps     | 3840x2160  | 3          | 666ec80235a1e8f59db044d77a89a495 |
| 3   | tile_C         | # of Tiles: max num   tile (20x20)                           | 422-10              | 5     | 0    | 30 fps     | 7680x4320  | 3          | 75363d036965a9dccc90a9ce8d0ae652 |
| 4   | tile_D         | tile dummy data test                                         | 422-10              | 4.1   | 2    | 60 fps     | 3840x2160  | 3          | dd492519c90409a9ca5710746f45c125 |
| 5   | tile_E         | tile_size_present_in_fh_flag=on                              | 422-10              | 4.1   | 2    | 60 fps     | 3840x2160  | 3          | 134c4aa46cec9ab0299824682a89eecd |
| 6   | qp_A           | QP matrix enabled                                            | 422-10              | 4.1   | 2    | 60 fps     | 3840x2160  | 3          | 5ca6d4ea0f65add261b44ed3532a0a73 |
| 7   | qp_B           | Tile QP   variation in a frame                               | 422-10              | 4.1   | 2    | 60 fps     | 3840x2160  | 3          | 85bfa477911447d994c17dea9703a9c7 |
| 8   | qp_C           | Set all the QPs in a   frame equal to min. QP (=0)           | 422-10              | 6     | 2    | 60 fps     | 3840x2160  | 3          | 8c2928ec05eb06d42d6a8bda0ceb7e8d |
| 9   | qp_D           | Set all the QPs in a   frame equal to max. QP (=51)          | 422-10              | 4.1   | 2    | 60 fps     | 3840x2160  | 3          | 9c98e376fb59100f5a5585482fb33746 |
| 10  | qp_E           | Set different QP   betwee luma and chroma                    | 422-10              | 4.1   | 2    | 60 fps     | 3840x2160  | 3          | 6d1a1bc982d412758f353c8d041979d1 |
| 11  | syn_A          | Exercise a synthetic   image with QP = 0 and QP = 51         | 422-10              | 4.1   | 2    | 60 fps     | 1920x1080  | 2          | db9f8f7ce57871481e5b257b79149b1e |
| 12  | syn_B          | Exercise a synthetic   image with Tile QP variation in Frame | 422-10              | 4.1   | 2    | 60 fps     | 1920x1080  | 2          | 5f6c57f0bfe7ceb2f97a56a3bec7fb7a |

## Test sequence
"sequence" folder has the uncompressed video sequence for encoder testing.
