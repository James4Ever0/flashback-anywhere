PYTHONUNBUFFERED=1 python -c "
import logging
logging.basicConfig(level=logging.DEBUG)
import sys
sys.path.insert(0, '.')
from client import main
main()
" -v /dev/ttyUSB0 upload test.jpg /picture100.jpg 2>&1