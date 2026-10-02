// SPDX-License-Identifier: MPL-2.0

import XCTest
@testable import OXRSysStreaming

final class PyroWaveDecoderTests: XCTestCase {
    private func header(width: UInt32, height: UInt32, extended: Bool) -> Data {
        var word = ((width - 1) & 0x3FFF) | (((height - 1) & 0x3FFF) << 14)
        if extended { word |= 1 << 31 }
        var bytes = withUnsafeBytes(of: word.littleEndian) { Data($0) }
        bytes.append(Data(count: 4))
        return bytes
    }

    func testFrameSizeReadsTheSequenceHeader() {
        let size = PyroWaveDecoder.frameSize(header(width: 2272, height: 1264, extended: true))
        XCTAssertEqual(size?.0, 2272)
        XCTAssertEqual(size?.1, 1264)
    }

    func testFrameSizeRefusesABlockHeaderOrShortData() {
        XCTAssertNil(PyroWaveDecoder.frameSize(header(width: 2272, height: 1264, extended: false)))
        XCTAssertNil(PyroWaveDecoder.frameSize(Data([0xFF, 0xFF, 0xFF])))
    }

    func testVideoCodecValuesMatchTheCppEnum() {
        XCTAssertEqual(VideoCodec.cineForm.rawValue, 3)
        XCTAssertEqual(VideoCodec.pyroWave.rawValue, 4)
    }
}
