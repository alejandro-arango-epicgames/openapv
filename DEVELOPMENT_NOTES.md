# Development Notes

## Selective Tile Decoding Implementation

### Status
- ✅ Core selective tile decoding functionality implemented and tested
- ✅ API functions: `oapvd_decode_selective()` and `oapvd_decode_selective_multi()`
- ✅ Test infrastructure with Python visualization tools
- ✅ Output organization to test/output/ directory
- ✅ Comprehensive testing with koala sample

### Future Cleanup Tasks

#### Debug Output Removal
The current implementation contains extensive debug printf statements in `src/oapv.c` that were added during development for troubleshooting. These should be removed in a future commit:

**Debug statements to remove:**
- Block-level debug prints in `dec_block()` function (lines ~1533-1580)
- Tile component debug prints in `dec_tile_comp()` function (lines ~1669-1720)  
- Regular decoder debug prints in `dec_tile()` function (lines ~1750-1830)
- Selective decoder debug prints in `oapvd_decode_selective()` function (lines ~2447-2660)
- Multi-tile decoder debug prints in `oapvd_decode_selective_multi()` function

**Note:** Debug cleanup was attempted but proved risky due to complex conditional debug blocks. Manual cleanup recommended to avoid breaking functional code.

### Implementation Notes
- Selective decoding currently uses full-frame decode + tile extraction approach
- This ensures correctness but is not optimal for performance
- Future optimization could implement true selective tile decoding
- Test infrastructure validates correctness against full decoder output