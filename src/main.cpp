#include "mf_examples.h"

using namespace nakamir;

// Only one scenario may be run at a time
int main(void) {
	// SCENARIO 1: Decode an MP4 file from a local or online source as fast as possible
	//mf_decode_from_url(L"http://commondatastorage.googleapis.com/gtv-videos-bucket/sample/BigBuckBunny.mp4");

	// SCENARIO 2: Read from the webcam, encode the sample, decode the sample, and render
	mf_roundtrip_webcam();
	return 0;
}
