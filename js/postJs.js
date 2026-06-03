  delete window.Module;
  
  self.surftcl = {
    onReady: function(callback) {
      if (Module["calledRun"] != undefined && Module["calledRun"]) {
        callback(_Result);
      } else {
        _OnReadyCb = callback;
      }
    }
  };
  
})(this);
